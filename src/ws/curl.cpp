#include "../http/curl_common.hpp"
#include "../net/platform.hpp"
#include "../ws_internal.hpp"

#include "borealis/log.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr borealis::Log Log{"borealis::ws"};
}

namespace borealis::ws::detail {
namespace {
using Clock = std::chrono::steady_clock;
namespace platform = borealis::net::detail::platform;

struct Handshake {
    int status = 0;
    std::vector<http::Header> headers;
};

size_t header_callback(char* data, size_t size, size_t count, void* userData) noexcept try {
    const size_t bytes = size * count;
    auto& handshake = *static_cast<Handshake*>(userData);
    const std::string_view line{data, bytes};
    if (line.starts_with("HTTP/")) {
        handshake.status = borealis::detail::curl::status_code(line);
        handshake.headers.clear();
    } else if (auto header = borealis::detail::curl::header(line)) {
        handshake.headers.emplace_back(std::move(*header));
    }
    return bytes;
}
BOREALIS_CATCH_RETURN(0)

bool has_wss_protocol() {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr || info->protocols == nullptr) {
        return false;
    }
    for (const char* const* protocol = info->protocols; *protocol != nullptr; ++protocol) {
        if (std::strcmp(*protocol, "wss") == 0) {
            return true;
        }
    }
    return false;
}

class CurlTransport final : public Transport {
public:
    ~CurlTransport() override { abort(); }

    void start(const Options& value, std::shared_ptr<EventSink> eventSink) override {
        options = value;
        sink = std::move(eventSink);
        platformInitialized = platform::initialize();
        if (!platformInitialized || !wakeup.open()) {
            release_platform();
            sink->closed(Error::Network, "Failed to create WebSocket wakeup", 0, 0, {});
            return;
        }
        try {
            worker = std::thread{[this] { run_guarded(); }};
        } catch (const std::exception& error) {
            wakeup.close();
            release_platform();
            sink->closed(Error::Network, error.what(), 0, 0, {});
        }
    }

    bool send(MessageKind kind, std::string data) override {
        std::lock_guard lock{mutex};
        if (stopping || closeRequested) {
            return false;
        }
        outgoing.push_back({kind, std::move(data)});
        wake();
        return true;
    }

    void close(uint16_t code, std::string reason) override {
        std::lock_guard lock{mutex};
        if (stopping || closeRequested) {
            return;
        }
        closeRequested = true;
        closeCode = code;
        closeReason = std::move(reason);
        wake();
    }

    void abort() noexcept override {
        {
            std::lock_guard lock{mutex};
            if (stopping && !worker.joinable()) {
                wakeup.close();
                release_platform();
                return;
            }
            stopping = true;
            wake();
        }
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            try {
                worker.join();
            }
            BOREALIS_CATCH_FATAL()
        }
        wakeup.close();
        release_platform();
    }

private:
    struct Outgoing {
        MessageKind kind;
        std::string data;
        size_t offset = 0;
    };

    static int progress_callback(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return static_cast<CurlTransport*>(userData)->should_stop() ? 1 : 0;
    }

    bool should_stop() {
        std::lock_guard lock{mutex};
        return stopping;
    }

    void wake() noexcept { wakeup.wake(); }

    void release_platform() noexcept {
        if (std::exchange(platformInitialized, false)) {
            platform::cleanup();
        }
    }

    void fail(Error error, std::string message) {
        if (!should_stop()) {
            sink->closed(error, std::move(message), 0, 0, {});
        }
    }

    void report_worker_failure(std::string_view message) noexcept try {
        fail(Error::Network, std::string{message});
    }
    BOREALIS_CATCH()

    void run_guarded() noexcept try { run(); } catch (const std::exception& exception) {
        ::Log.error("{}: {}", __func__, exception.what());
        report_worker_failure(exception.what());
    } catch (...) {
        ::Log.error("{}: unknown exception", __func__);
        report_worker_failure("WebSocket worker failed");
    }

    void run() {
        borealis::detail::curl::initialize();

        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            fail(Error::Network, "Failed to create libcurl WebSocket connection");
            return;
        }
        borealis::detail::curl::Headers requestHeaders;
        for (const auto& header : options.headers) {
            if (!requestHeaders.append(header.name + ": " + header.value)) {
                curl_easy_cleanup(curl);
                fail(Error::Network, "Failed to allocate WebSocket headers");
                return;
            }
        }

        Handshake handshake;
        std::array<char, CURL_ERROR_SIZE> errorBuffer{};
        curl_easy_setopt(curl, CURLOPT_URL, options.url.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, requestHeaders.get());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
            borealis::detail::curl::timeout_ms(options.connectTimeout));
        curl_easy_setopt(
            curl, CURLOPT_TIMEOUT_MS, borealis::detail::curl::timeout_ms(options.connectTimeout));
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &handshake);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer.data());
#if CURL_AT_LEAST_VERSION(7, 85, 0)
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "ws,wss");
#endif
        const CURLcode connected = curl_easy_perform(curl);
        if (should_stop()) {
            curl_easy_cleanup(curl);
            return;
        }
        long status = handshake.status;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status != 0 && status != 101) {
            const std::string message =
                errorBuffer[0] != '\0' ? errorBuffer.data() : "WebSocket upgrade was refused";
            sink->closed(Error::Handshake, message, static_cast<int>(status), 0, {},
                std::move(handshake.headers));
            curl_easy_cleanup(curl);
            return;
        }
        if (connected != CURLE_OK) {
            const Error error =
                connected == CURLE_OPERATION_TIMEDOUT ? Error::Timeout : Error::Network;
            fail(
                error, errorBuffer[0] != '\0' ? errorBuffer.data() : curl_easy_strerror(connected));
            curl_easy_cleanup(curl);
            return;
        }
        if (status != 101) {
            sink->closed(Error::Handshake, "WebSocket upgrade did not return status 101",
                static_cast<int>(status), 0, {}, std::move(handshake.headers));
            curl_easy_cleanup(curl);
            return;
        }

        curl_socket_t socket = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
            socket == CURL_SOCKET_BAD)
        {
            fail(Error::Network, "libcurl did not expose the WebSocket descriptor");
            curl_easy_cleanup(curl);
            return;
        }
        sink->opened(selected_protocol(handshake.headers), std::move(handshake.headers));
        io_loop(curl, socket);
        curl_easy_cleanup(curl);
    }

    void io_loop(CURL* curl, curl_socket_t socket) {
        auto nextPing = options.keepaliveInterval.count() != 0 ?
                            Clock::now() + options.keepaliveInterval :
                            Clock::time_point::max();

        while (!should_stop()) {
            // libcurl can retain frames received with the upgrade response. Drain its buffer
            // before waiting on the descriptor, which may no longer be readable in that case.
            if (!receive_available(curl)) {
                return;
            }
            short socketEvents = POLLIN;
            {
                std::lock_guard lock{mutex};
                if (!outgoing.empty() || (closeRequested && !closeSent)) {
                    socketEvents |= POLLOUT;
                }
            }
            platform::PollDescriptor descriptors[]{
                {wakeup.descriptor(), POLLIN, 0},
                {static_cast<platform::NativeSocket>(socket), socketEvents, 0},
            };
            const int timeout = nextPing == Clock::time_point::max() ?
                                    1000 :
                                    static_cast<int>(std::clamp<int64_t>(
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                            nextPing - Clock::now())
                                            .count(),
                                        0, 1000));
            const int result = platform::poll(descriptors, std::chrono::milliseconds{timeout});
            if (result < 0 && !platform::interrupted(platform::last_socket_error())) {
                fail(Error::Network, platform::socket_error_message(platform::last_socket_error()));
                return;
            }
            if ((descriptors[0].revents & POLLIN) != 0) {
                wakeup.drain();
            }
            if ((descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
                fail(Error::Network, "WebSocket descriptor failed");
                return;
            }
            if ((descriptors[1].revents & POLLOUT) != 0) {
                if (!send_queued(curl)) {
                    return;
                }
                if (peerCloseReceived && closeSent) {
                    sink->closed(Error::None, {}, 0, peerCloseCode, std::move(peerCloseReason));
                    return;
                }
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
                if (!receive_available(curl)) {
                    return;
                }
            }
            bool canPing = true;
            {
                std::lock_guard lock{mutex};
                canPing = !closeRequested;
            }
            if (canPing && Clock::now() >= nextPing) {
                size_t sent = 0;
                const CURLcode result = curl_ws_send(curl, "", 0, &sent, 0, CURLWS_PING);
                if (result != CURLE_OK && result != CURLE_AGAIN) {
                    fail(Error::Network, curl_easy_strerror(result));
                    return;
                }
                nextPing = Clock::now() + options.keepaliveInterval;
            }
        }
    }

    bool send_queued(CURL* curl) {
        for (;;) {
            std::unique_lock lock{mutex};
            if (outgoing.empty()) {
                if (!closeRequested || closeSent) {
                    return true;
                }
                if (!closeFramePrepared) {
                    closeFrame.push_back(static_cast<char>(closeCode >> 8));
                    closeFrame.push_back(static_cast<char>(closeCode & 0xff));
                    closeFrame += closeReason;
                    closeFramePrepared = true;
                }
                lock.unlock();
                size_t sent = 0;
                const CURLcode result = curl_ws_send(curl, closeFrame.data() + closeOffset,
                    closeFrame.size() - closeOffset, &sent, 0, CURLWS_CLOSE);
                closeOffset += std::min(sent, closeFrame.size() - closeOffset);
                if (result != CURLE_OK && result != CURLE_AGAIN) {
                    fail(Error::Network, curl_easy_strerror(result));
                    return false;
                }
                if (closeOffset == closeFrame.size()) {
                    closeSent = true;
                }
                return true;
            }
            Outgoing& message = outgoing.front();
            const size_t remaining = message.data.size() - message.offset;
            const int flags = message.kind == MessageKind::Text ? CURLWS_TEXT : CURLWS_BINARY;
            lock.unlock();
            size_t sent = 0;
            const CURLcode result = curl_ws_send(
                curl, message.data.data() + message.offset, remaining, &sent, 0, flags);
            if (result != CURLE_OK && result != CURLE_AGAIN) {
                fail(Error::Network, curl_easy_strerror(result));
                return false;
            }
            lock.lock();
            message.offset += std::min(sent, remaining);
            if (message.offset != message.data.size()) {
                return true;
            }
            const size_t completed = message.data.size();
            outgoing.pop_front();
            lock.unlock();
            sink->send_complete(completed, Error::None, {});
        }
    }

    bool receive_available(CURL* curl) {
        std::array<char, 64 * 1024> buffer;
        for (size_t iteration = 0; iteration < 16; ++iteration) {
            size_t received = 0;
            const curl_ws_frame* frame = nullptr;
            const CURLcode result =
                curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &frame);
            if (result == CURLE_AGAIN) {
                return true;
            }
            if (result == CURLE_GOT_NOTHING) {
                sink->closed(
                    Error::Network, "WebSocket peer closed without a close frame", 0, 0, {});
                return false;
            }
            if (result != CURLE_OK || frame == nullptr) {
                fail(Error::Network, curl_easy_strerror(result));
                return false;
            }
            if ((frame->flags & CURLWS_CLOSE) != 0) {
                std::string payload{buffer.data(), received};
                uint16_t code = 0;
                std::string reason;
                if (payload.size() >= 2) {
                    code = (static_cast<unsigned char>(payload[0]) << 8) |
                           static_cast<unsigned char>(payload[1]);
                    reason = payload.substr(2);
                }
                if (closeSent) {
                    sink->closed(Error::None, {}, 0, code, std::move(reason));
                    return false;
                }
                {
                    std::lock_guard lock{mutex};
                    closeRequested = true;
                    if (!outgoing.empty() && outgoing.front().offset != 0) {
                        outgoing.erase(std::next(outgoing.begin()), outgoing.end());
                    } else {
                        outgoing.clear();
                    }
                }
                closeFrame = std::move(payload);
                closeOffset = 0;
                closeFramePrepared = true;
                peerCloseReceived = true;
                peerCloseCode = code;
                peerCloseReason = std::move(reason);
                return true;
            }
            if ((frame->flags & (CURLWS_PING | CURLWS_PONG)) != 0) {
                continue;
            }
            if ((frame->flags & CURLWS_TEXT) != 0) {
                incomingKind = MessageKind::Text;
            } else if ((frame->flags & CURLWS_BINARY) != 0) {
                incomingKind = MessageKind::Binary;
            } else {
                sink->closed(Error::Protocol, "WebSocket frame has no message type", 0, 0, {});
                return false;
            }
            incoming.append(buffer.data(), received);
            if (incoming.size() > options.maxMessageBytes) {
                sink->message(incomingKind, std::move(incoming));
                return true;
            }
            if (frame->bytesleft == 0 && (frame->flags & CURLWS_CONT) == 0) {
                sink->message(incomingKind, std::move(incoming));
                incoming.clear();
            }
        }
        return true;
    }

    Options options;
    std::shared_ptr<EventSink> sink;
    std::mutex mutex;
    std::deque<Outgoing> outgoing;
    std::thread worker;
    std::string incoming;
    MessageKind incomingKind = MessageKind::Binary;
    std::string closeReason;
    std::string closeFrame;
    std::string peerCloseReason;
    size_t closeOffset = 0;
    uint16_t closeCode = 1000;
    uint16_t peerCloseCode = 0;
    platform::Wakeup wakeup;
    bool platformInitialized = false;
    bool stopping = false;
    bool closeRequested = false;
    bool closeSent = false;
    bool closeFramePrepared = false;
    bool peerCloseReceived = false;
};

}  // namespace

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<CurlTransport>();
}

bool backend_available() noexcept {
    static const bool available = has_wss_protocol();
    return available;
}

}  // namespace borealis::ws::detail
