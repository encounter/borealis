#include "../http/winhttp_common.hpp"
#include "../ws_internal.hpp"

#include "borealis/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr borealis::Log Log{"borealis::ws"};
}

namespace borealis::ws::detail {
namespace {
namespace winhttp = borealis::detail::winhttp;

Error map_error(DWORD error) {
    return winhttp::map_error<Error>(error);
}

class WinHttpTransport final : public Transport {
public:
    ~WinHttpTransport() override { abort(); }

    void start(const Options& value, std::shared_ptr<EventSink> eventSink) override {
        std::lock_guard lock{mutex};
        options = value;
        sink = std::move(eventSink);
        if (!open_locked()) {
            const DWORD error = GetLastError();
            terminal_locked(
                map_error(error), winhttp::error_message(error, "Failed to start WebSocket"));
        }
    }

    bool send(MessageKind kind, std::string data) override {
        std::lock_guard lock{mutex};
        if (terminal || websocket == nullptr || closeRequested ||
            data.size() > std::numeric_limits<DWORD>::max())
        {
            return false;
        }
        outgoing.push_back({kind, std::move(data)});
        start_send_locked();
        return true;
    }

    void close(uint16_t code, std::string reason) override {
        std::lock_guard lock{mutex};
        if (terminal || closeRequested) {
            return;
        }
        closeRequested = true;
        closeCode = code;
        closeReason = std::move(reason);
        start_close_locked();
    }

    void abort() noexcept override try {
        std::unique_lock lock{mutex};
        if (aborted) {
            return;
        }
        aborted = true;
        terminal = true;
        requestClosing = std::exchange(request, nullptr);
        websocketClosing = std::exchange(websocket, nullptr);
        HINTERNET requestHandle = requestClosing;
        HINTERNET websocketHandle = websocketClosing;
        lock.unlock();
        if (websocketHandle != nullptr && !WinHttpCloseHandle(websocketHandle)) {
            lock.lock();
            websocketClosed = true;
            callbacksClosed.notify_all();
            lock.unlock();
        }
        if (requestHandle != nullptr && !WinHttpCloseHandle(requestHandle)) {
            lock.lock();
            requestClosed = true;
            callbacksClosed.notify_all();
            lock.unlock();
        }
        lock.lock();
        callbacksClosed.wait(lock, [this] {
            return (requestClosed || !requestWasStarted) &&
                   (websocketClosed || !websocketWasStarted);
        });
        lock.unlock();
        if (connection != nullptr) {
            WinHttpCloseHandle(connection);
            connection = nullptr;
        }
        if (session != nullptr) {
            WinHttpCloseHandle(session);
            session = nullptr;
        }
    }
    BOREALIS_CATCH()

    void on_status(
        HINTERNET handle, DWORD status, void* information, DWORD informationSize) noexcept try {
        std::lock_guard lock{mutex};
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            if (handle == requestClosing) {
                requestClosed = true;
            } else if (handle == websocketClosing) {
                websocketClosed = true;
            }
            callbacksClosed.notify_all();
            return;
        }
        if (terminal) {
            return;
        }
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            if (!WinHttpReceiveResponse(handle, nullptr)) {
                fail_last_locked("Failed to receive WebSocket upgrade");
            }
            break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            upgrade_locked(handle);
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            receive_complete_locked(information, informationSize);
            break;
        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
            send_complete_locked();
            break;
        case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
            close_complete_locked();
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            if (information != nullptr && informationSize >= sizeof(WINHTTP_ASYNC_RESULT)) {
                const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(information);
                fail_locked(map_error(result->dwError),
                    winhttp::error_message(result->dwError, "WinHTTP WebSocket failed"));
            } else {
                fail_locked(Error::Network, "WinHTTP WebSocket failed");
            }
            break;
        default:
            break;
        }
    } catch (const std::exception& exception) {
        ::Log.error("{}: {}", __func__, exception.what());
        fail_unexpected_callback();
    } catch (...) {
        ::Log.error("{}: unknown exception", __func__);
        fail_unexpected_callback();
    }

private:
    struct Outgoing {
        MessageKind kind;
        std::string data;
    };

    void fail_unexpected_callback() noexcept try {
        std::lock_guard lock{mutex};
        fail_locked(Error::Network, "WinHTTP WebSocket callback failed");
    }
    BOREALIS_CATCH()

    bool open_locked() {
        const std::string httpUrl = "http" + options.url.substr(2);
        const std::wstring wideUrl = winhttp::utf8_to_wide(httpUrl);
        if (wideUrl.empty()) {
            SetLastError(ERROR_WINHTTP_INVALID_URL);
            return false;
        }
        const auto cracked = winhttp::crack_url(wideUrl);
        if (!cracked) {
            return false;
        }

        session = WinHttpOpen(L"borealis", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (session == nullptr || !winhttp::configure_secure_protocols(session)) {
            return false;
        }
        const DWORD timeout = winhttp::timeout_ms(options.connectTimeout);
        WinHttpSetTimeouts(session, timeout, timeout, 0, 0);
        connection = WinHttpConnect(session, cracked->host.c_str(), cracked->port, 0);
        if (connection == nullptr) {
            return false;
        }
        request = WinHttpOpenRequest(connection, L"GET", cracked->path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            cracked->secure ? WINHTTP_FLAG_SECURE : 0);
        if (request == nullptr) {
            return false;
        }
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(
            request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            return false;
        }
        for (const auto& header : options.headers) {
            const std::wstring wideHeader =
                winhttp::utf8_to_wide(header.name + ": " + header.value);
            if (wideHeader.empty() ||
                !WinHttpAddRequestHeaders(request, wideHeader.c_str(),
                    static_cast<DWORD>(wideHeader.size()), WINHTTP_ADDREQ_FLAG_ADD))
            {
                return false;
            }
        }
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
            WinHttpSetStatusCallback(request, status_callback,
                WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                0) == WINHTTP_INVALID_STATUS_CALLBACK)
        {
            return false;
        }
        requestWasStarted = true;
        return WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                   WINHTTP_NO_REQUEST_DATA, 0, 0, context) != FALSE;
    }

    void upgrade_locked(HINTERNET handle) {
        int status = 0;
        std::vector<http::Header> headers;
        if (!winhttp::query_response(handle, status, headers)) {
            fail_last_locked("Failed to read WebSocket upgrade response");
            return;
        }
        if (status != 101) {
            terminal_locked(Error::Handshake, "WebSocket upgrade was refused", status, 0, {},
                std::move(headers));
            return;
        }
        websocket = WinHttpWebSocketCompleteUpgrade(handle, reinterpret_cast<DWORD_PTR>(this));
        if (websocket == nullptr) {
            fail_last_locked("Failed to complete WebSocket upgrade");
            return;
        }
        websocketWasStarted = true;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        WinHttpSetOption(websocket, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));
        if (options.keepaliveInterval.count() != 0) {
            DWORD keepalive = winhttp::timeout_ms(options.keepaliveInterval);
            WinHttpSetOption(websocket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepalive,
                sizeof(keepalive));
        }
        const std::string protocol = selected_protocol(headers);
        sink->opened(protocol, std::move(headers));
        begin_receive_locked();
    }

    void begin_receive_locked() {
        if (websocket == nullptr || receivePending) {
            return;
        }
        receivePending = true;
        const DWORD result = WinHttpWebSocketReceive(websocket, receiveBuffer.data(),
            static_cast<DWORD>(receiveBuffer.size()), nullptr, nullptr);
        if (result != NO_ERROR) {
            receivePending = false;
            fail_locked(map_error(result),
                winhttp::error_message(result, "Failed to receive WebSocket data"));
        }
    }

    void receive_complete_locked(void* information, DWORD informationSize) {
        receivePending = false;
        if (information == nullptr || informationSize < sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
            fail_locked(Error::Network, "WinHTTP returned invalid WebSocket receive status");
            return;
        }
        const auto* status = static_cast<const WINHTTP_WEB_SOCKET_STATUS*>(information);
        const size_t bytes = status->dwBytesTransferred;
        if (bytes > receiveBuffer.size()) {
            fail_locked(Error::Network, "WinHTTP returned an oversized receive buffer");
            return;
        }
        switch (status->eBufferType) {
        case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
        case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
            incomingKind = MessageKind::Text;
            incoming.append(receiveBuffer.data(), bytes);
            break;
        case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
        case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
            incomingKind = MessageKind::Binary;
            incoming.append(receiveBuffer.data(), bytes);
            break;
        case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
            receive_close_locked();
            return;
        default:
            fail_locked(Error::Protocol, "WinHTTP returned an unknown WebSocket buffer type");
            return;
        }
        if (incoming.size() > options.maxMessageBytes) {
            sink->message(incomingKind, std::move(incoming));
            incoming.clear();
            return;
        }
        if (status->eBufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            status->eBufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
        {
            sink->message(incomingKind, std::move(incoming));
            incoming.clear();
        }
        begin_receive_locked();
    }

    void start_send_locked() {
        if (sendPending || websocket == nullptr || outgoing.empty()) {
            return;
        }
        const auto& message = outgoing.front();
        const auto type = message.kind == MessageKind::Text ?
                              WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE :
                              WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD result = WinHttpWebSocketSend(websocket, type,
            const_cast<char*>(message.data.data()), static_cast<DWORD>(message.data.size()));
        if (result != NO_ERROR) {
            fail_locked(
                map_error(result), winhttp::error_message(result, "Failed to send WebSocket data"));
            return;
        }
        sendPending = true;
    }

    void send_complete_locked() {
        if (!sendPending || outgoing.empty()) {
            return;
        }
        sendPending = false;
        const size_t bytes = outgoing.front().data.size();
        outgoing.pop_front();
        sink->send_complete(bytes, Error::None, {});
        if (peerCloseReceived) {
            outgoing.clear();
        } else {
            start_send_locked();
        }
        start_close_locked();
    }

    void start_close_locked() {
        if (!closeRequested || closePending || shutdownStarted || sendPending ||
            !outgoing.empty() || websocket == nullptr)
        {
            return;
        }
        if (peerCloseReceived) {
            const DWORD result = WinHttpWebSocketClose(websocket, peerCloseCode,
                const_cast<char*>(peerCloseReason.data()),
                static_cast<DWORD>(peerCloseReason.size()));
            if (result != NO_ERROR) {
                fail_locked(
                    map_error(result), winhttp::error_message(result, "Failed to close WebSocket"));
                return;
            }
            closePending = true;
        } else {
            const DWORD result = WinHttpWebSocketShutdown(websocket, closeCode,
                const_cast<char*>(closeReason.data()), static_cast<DWORD>(closeReason.size()));
            if (result != NO_ERROR && result != ERROR_IO_PENDING) {
                fail_locked(map_error(result),
                    winhttp::error_message(result, "Failed to shut down WebSocket"));
                return;
            }
            shutdownStarted = true;
        }
    }

    void receive_close_locked() {
        std::array<char, 124> reason{};
        DWORD reasonBytes = static_cast<DWORD>(reason.size());
        USHORT code = 0;
        const DWORD result = WinHttpWebSocketQueryCloseStatus(
            websocket, &code, reason.data(), reasonBytes, &reasonBytes);
        if (result != NO_ERROR) {
            fail_locked(map_error(result),
                winhttp::error_message(result, "Failed to read WebSocket close status"));
            return;
        }
        peerCloseReceived = true;
        peerCloseCode = code;
        peerCloseReason.assign(reason.data(), static_cast<size_t>(reasonBytes));
        closeRequested = true;

        if (sendPending && !outgoing.empty()) {
            outgoing.erase(std::next(outgoing.begin()), outgoing.end());
        } else {
            outgoing.clear();
        }
        if (shutdownStarted) {
            terminal_locked(Error::None, {}, 0, peerCloseCode, std::move(peerCloseReason));
        } else {
            start_close_locked();
        }
    }

    void close_complete_locked() {
        closePending = false;
        terminal_locked(Error::None, {}, 0, peerCloseCode, std::move(peerCloseReason));
    }

    void fail_last_locked(std::string_view operation) {
        const DWORD error = GetLastError();
        fail_locked(map_error(error), winhttp::error_message(error, operation));
    }

    void fail_locked(Error error, std::string message) {
        terminal_locked(error, std::move(message));
    }

    void terminal_locked(Error error, std::string message, int status = 0, uint16_t code = 0,
        std::string reason = {}, std::vector<http::Header> headers = {}) {
        if (terminal) {
            return;
        }
        terminal = true;
        sink->closed(
            error, std::move(message), status, code, std::move(reason), std::move(headers));
    }

    static void CALLBACK status_callback(HINTERNET handle, DWORD_PTR context, DWORD status,
        void* information, DWORD informationSize) {
        if (context != 0) {
            reinterpret_cast<WinHttpTransport*>(context)->on_status(
                handle, status, information, informationSize);
        }
    }

    Options options;
    std::shared_ptr<EventSink> sink;
    std::recursive_mutex mutex;
    std::condition_variable_any callbacksClosed;
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    HINTERNET websocket = nullptr;
    HINTERNET requestClosing = nullptr;
    HINTERNET websocketClosing = nullptr;
    std::deque<Outgoing> outgoing;
    std::array<char, 64 * 1024> receiveBuffer{};
    std::string incoming;
    std::string closeReason;
    std::string peerCloseReason;
    MessageKind incomingKind = MessageKind::Binary;
    uint16_t closeCode = 1000;
    uint16_t peerCloseCode = 0;
    bool sendPending = false;
    bool receivePending = false;
    bool closeRequested = false;
    bool closePending = false;
    bool shutdownStarted = false;
    bool peerCloseReceived = false;
    bool terminal = false;
    bool aborted = false;
    bool requestWasStarted = false;
    bool websocketWasStarted = false;
    bool requestClosed = false;
    bool websocketClosed = false;
};

}  // namespace

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<WinHttpTransport>();
}

bool backend_available() noexcept {
    return true;
}

}  // namespace borealis::ws::detail
