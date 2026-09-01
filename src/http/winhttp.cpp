#include "borealis/http.hpp"

#include "../http_internal.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace borealis::http {
namespace {

struct WinHttpHandle {
    HINTERNET handle = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET value) : handle{value} {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    ~WinHttpHandle() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }

    HINTERNET release() noexcept {
        HINTERNET value = handle;
        handle = nullptr;
        return value;
    }

    operator HINTERNET() const { return handle; }
};

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), required);
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
        required, nullptr, nullptr);
    return result;
}

DWORD timeout_ms(std::chrono::milliseconds timeout) {
    const auto count = std::max<std::chrono::milliseconds::rep>(1, timeout.count());
    return static_cast<DWORD>(
        std::min<std::chrono::milliseconds::rep>(count, std::numeric_limits<int>::max()));
}

Error map_winhttp_error(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:
        return Error::Timeout;
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return Error::Canceled;
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return Error::InvalidUrl;
    default:
        return Error::Network;
    }
}

detail::TransportResult fail_from_error(DWORD error, const char* message) {
    return {
        .error = map_winhttp_error(error),
        .message = std::string{message} + " (" + std::to_string(error) + ")",
    };
}

detail::TransportResult fail_from_last_error(const char* message) {
    return fail_from_error(GetLastError(), message);
}

void parse_headers(std::wstring_view rawHeaders, std::vector<Header>& headers) {
    size_t start = 0;
    bool firstLine = true;
    while (start < rawHeaders.size()) {
        size_t end = rawHeaders.find(L"\r\n", start);
        if (end == std::wstring_view::npos) {
            end = rawHeaders.size();
        }

        const std::wstring_view line = rawHeaders.substr(start, end - start);
        if (!line.empty() && !firstLine) {
            const size_t colon = line.find(L':');
            if (colon != std::wstring_view::npos) {
                headers.push_back({
                    .name = wide_to_utf8(line.substr(0, colon)),
                    .value = detail::trim_header_value(wide_to_utf8(line.substr(colon + 1))),
                });
            }
        }
        firstLine = false;

        if (end == rawHeaders.size()) {
            break;
        }
        start = end + 2;
    }
}

bool read_response(HINTERNET request, int& status, std::vector<Header>& headers) {
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
    {
        return false;
    }
    status = static_cast<int>(statusCode);

    DWORD headerBytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER, &headerBytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }

    std::wstring rawHeaders(headerBytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
            rawHeaders.data(), &headerBytes, WINHTTP_NO_HEADER_INDEX))
    {
        return false;
    }
    if (!rawHeaders.empty() && rawHeaders.back() == L'\0') {
        rawHeaders.pop_back();
    }
    parse_headers(rawHeaders, headers);
    return true;
}

class AsyncRequest {
public:
    AsyncRequest(detail::Deadline& deadline, detail::TransportObserver& observer,
        borealis::detail::TaskSignals* signals, HINTERNET requestHandle)
        : m_deadline{deadline}, m_observer{observer}, m_signals{signals},
          m_requestHandle{requestHandle} {}

    detail::TransportResult run(void* requestBody, DWORD requestBodySize) {
        {
            std::lock_guard lock{m_mutex};
            if (!WinHttpSendRequest(m_requestHandle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, requestBody,
                    requestBodySize, requestBodySize, reinterpret_cast<DWORD_PTR>(this)))
            {
                fail_from_last_error_locked("Failed to send request");
            }
        }

        wait_for_completion();
        close_request();

        std::lock_guard lock{m_mutex};
        return {
            .error = m_error,
            .message = std::move(m_message),
        };
    }

    void on_status(
        HINTERNET requestHandle, DWORD status, void* information, DWORD informationSize) noexcept {
        try {
            std::lock_guard lock{m_mutex};
            if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
                m_handleClosed = true;
                m_changed.notify_all();
                return;
            }
            if (m_closing || m_complete) {
                return;
            }

            switch (status) {
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                m_activity.touch();
                if (!WinHttpReceiveResponse(requestHandle, nullptr)) {
                    fail_from_last_error_locked("Failed to receive response");
                }
                break;
            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                receive_headers_locked(requestHandle);
                break;
            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                receive_data_locked(requestHandle, information, informationSize);
                break;
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
                if (information != nullptr && informationSize >= sizeof(WINHTTP_ASYNC_RESULT)) {
                    const auto* asyncResult = static_cast<const WINHTTP_ASYNC_RESULT*>(information);
                    fail_from_error_locked(asyncResult->dwError, "WinHTTP request failed");
                } else {
                    fail_locked(Error::Network, "WinHTTP request failed");
                }
                break;
            default:
                break;
            }
        } catch (...) {
            fail_unexpected_callback();
        }
    }

private:
    void receive_headers_locked(HINTERNET requestHandle) {
        m_activity.start_response();
        int status = 0;
        std::vector<Header> headers;
        if (!read_response(requestHandle, status, headers)) {
            fail_from_last_error_locked("Failed to read response headers");
            return;
        }
        if (m_observer.on_response(status, std::move(headers)) ==
            detail::TransportObserver::Directive::Abort)
        {
            complete_locked();
            return;
        }
        begin_read_locked(requestHandle);
    }

    void receive_data_locked(HINTERNET requestHandle, void* information, DWORD bytesRead) {
        m_activity.touch();
        if (bytesRead == 0) {
            complete_locked();
            return;
        }
        if (information == nullptr || bytesRead > m_buffer.size()) {
            fail_locked(Error::Network, "WinHTTP returned an invalid response buffer");
            return;
        }

        const auto chunk =
            std::span{static_cast<const std::byte*>(information), static_cast<size_t>(bytesRead)};
        if (m_observer.on_data(chunk) == detail::TransportObserver::Directive::Abort) {
            complete_locked();
            return;
        }
        begin_read_locked(requestHandle);
    }

    void begin_read_locked(HINTERNET requestHandle) {
        if (!WinHttpReadData(
                requestHandle, m_buffer.data(), static_cast<DWORD>(m_buffer.size()), nullptr))
        {
            fail_from_last_error_locked("Failed to read response body");
        }
    }

    void wait_for_completion() {
        std::unique_lock lock{m_mutex};
        while (!m_complete) {
            m_changed.wait_for(lock, detail::Deadline::PollInterval);
            if (m_complete) {
                break;
            }

            if (m_signals->cancelRequested.load(std::memory_order_relaxed)) {
                fail_locked(Error::Canceled, "Request canceled");
            } else if (m_deadline.expired(m_activity)) {
                fail_locked(Error::Timeout, "Request timed out");
            }
        }
    }

    void close_request() {
        HINTERNET requestHandle = nullptr;
        {
            std::lock_guard lock{m_mutex};
            m_closing = true;
            requestHandle = std::exchange(m_requestHandle, nullptr);
        }
        if (requestHandle == nullptr) {
            return;
        }

        // HANDLE_CLOSING fences the stack-owned callback state after cancellation.
        const bool closed = WinHttpCloseHandle(requestHandle) != FALSE;
        const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
        std::unique_lock lock{m_mutex};
        if (!closed) {
            m_handleClosed = true;
            if (m_error == Error::None) {
                m_error = map_winhttp_error(closeError);
                m_message = "Failed to close WinHTTP request (" + std::to_string(closeError) + ")";
            }
        }
        m_changed.wait(lock, [this] { return m_handleClosed; });
    }

    void complete_locked() {
        m_complete = true;
        m_changed.notify_all();
    }

    void fail_locked(Error error, const char* message) {
        if (m_complete) {
            return;
        }
        m_error = error;
        m_message = message;
        complete_locked();
    }

    void fail_from_error_locked(DWORD error, const char* message) {
        if (m_complete) {
            return;
        }
        m_error = map_winhttp_error(error);
        m_message = std::string{message} + " (" + std::to_string(error) + ")";
        complete_locked();
    }

    void fail_from_last_error_locked(const char* message) {
        fail_from_error_locked(GetLastError(), message);
    }

    void fail_unexpected_callback() noexcept {
        try {
            std::lock_guard lock{m_mutex};
            if (!m_complete) {
                m_error = Error::Network;
                try {
                    m_message = "WinHTTP callback failed";
                } catch (...) {
                    m_message.clear();
                }
                complete_locked();
            }
        } catch (...) {
        }
    }

    detail::Deadline& m_deadline;
    detail::TransportObserver& m_observer;
    borealis::detail::TaskSignals* m_signals;
    HINTERNET m_requestHandle;
    detail::Deadline::Tracker m_activity;
    std::recursive_mutex m_mutex;
    std::condition_variable_any m_changed;
    std::array<char, 64 * 1024> m_buffer;
    Error m_error = Error::None;
    std::string m_message;
    bool m_complete = false;
    bool m_closing = false;
    bool m_handleClosed = false;
};

void CALLBACK request_status_callback(HINTERNET requestHandle, DWORD_PTR context, DWORD status,
    void* information, DWORD informationSize) {
    if (context != 0) {
        reinterpret_cast<AsyncRequest*>(context)->on_status(
            requestHandle, status, information, informationSize);
    }
}

}  // namespace

bool available() noexcept {
    return true;
}

Backend backend() noexcept {
    return Backend::WinHttp;
}

const char* backend_name() noexcept {
    return "WinHTTP";
}

detail::TransportResult detail::send_request(const TransportRequest& request) {
    std::wstring wideUrl = utf8_to_wide(request.url);
    if (wideUrl.empty()) {
        return {
            .error = Error::InvalidUrl,
            .message = "URL is not valid UTF-8",
        };
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components)) {
        return fail_from_last_error("Failed to parse URL");
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS) {
        return {
            .error = Error::UnsupportedScheme,
            .message = "Only https:// URLs are supported",
        };
    }

    const std::wstring host{components.lpszHostName, components.dwHostNameLength};
    std::wstring path;
    if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    WinHttpHandle session{WinHttpOpen(L"borealis", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (session.handle == nullptr) {
        return fail_from_last_error("Failed to create WinHTTP session");
    }

    const DWORD connectTimeout =
        timeout_ms(request.deadline->bounded_timeout(request.deadline->connect_timeout()));
    const DWORD idleTimeout =
        timeout_ms(request.deadline->bounded_timeout(request.deadline->idle_timeout()));
    WinHttpSetTimeouts(session, connectTimeout, connectTimeout, idleTimeout, idleTimeout);

    WinHttpHandle connection{WinHttpConnect(session, host.c_str(), components.nPort, 0)};
    if (connection.handle == nullptr) {
        return fail_from_last_error("Failed to connect");
    }

    const size_t bodySize =
        detail::method_has_request_body(request.method) ? request.body.size() : 0;
    if (bodySize > std::numeric_limits<DWORD>::max()) {
        return {
            .error = Error::TooLarge,
            .message = "Request body is too large for WinHTTP",
        };
    }

    const std::wstring method = utf8_to_wide(detail::method_name(request.method));
    WinHttpHandle httpRequest{WinHttpOpenRequest(connection, method.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (httpRequest.handle == nullptr) {
        return fail_from_last_error("Failed to create request");
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(
        httpRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    DWORD maxRedirects = 20;
    WinHttpSetOption(httpRequest, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects,
        sizeof(maxRedirects));

    if (request.allowCompression) {
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(
            httpRequest, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    }

    for (const Header& header : request.headers) {
        const std::wstring wideHeader = utf8_to_wide(header.name + ": " + header.value);
        if (wideHeader.empty()) {
            return {
                .error = Error::InvalidUrl,
                .message = "Request header is not valid UTF-8",
            };
        }
        if (!WinHttpAddRequestHeaders(httpRequest, wideHeader.c_str(),
                static_cast<DWORD>(wideHeader.size()), WINHTTP_ADDREQ_FLAG_ADD))
        {
            return fail_from_last_error("Failed to add request header");
        }
    }

    if (request.signals->cancelRequested.load(std::memory_order_relaxed)) {
        return {
            .error = Error::Canceled,
            .message = "Request canceled",
        };
    }

    AsyncRequest asyncRequest{
        *request.deadline, *request.observer, request.signals, httpRequest.handle};
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&asyncRequest);
    if (!WinHttpSetOption(httpRequest, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
        return fail_from_last_error("Failed to set WinHTTP request context");
    }
    if (WinHttpSetStatusCallback(httpRequest, request_status_callback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
            0) == WINHTTP_INVALID_STATUS_CALLBACK)
    {
        return fail_from_last_error("Failed to set WinHTTP request callback");
    }

    httpRequest.release();
    void* requestBody =
        bodySize == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data());
    return asyncRequest.run(requestBody, static_cast<DWORD>(bodySize));
}

}  // namespace borealis::http
