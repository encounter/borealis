#include "borealis/http.hpp"

#include "../http_internal.hpp"
#include "borealis/log.hpp"
#include "curl_common.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <string_view>

namespace {
constexpr borealis::Log Log{"borealis::http"};
}

namespace borealis::http {
namespace {
struct CurlContext {
    detail::TransportObserver* observer = nullptr;
    borealis::detail::TaskSignals* signals = nullptr;
    detail::Deadline* deadline = nullptr;
    detail::Deadline::Tracker activity;
    std::vector<Header> headers;
    curl_off_t lastBytesRead = 0;
    curl_off_t lastBytesWritten = 0;
    int currentStatus = 0;
    bool callbackFailed = false;
    bool timedOut = false;
};

template <typename Fn>
size_t guarded_callback(CurlContext& context, std::string_view operation, Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::exception& exception) {
        ::Log.error("{}: {}", operation, exception.what());
    } catch (...) {
        ::Log.error("{}: unknown exception", operation);
    }
    context.callbackFailed = true;
    return 0;
}

bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

size_t write_body_impl(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* context = static_cast<CurlContext*>(userdata);
    const size_t bytes = size * nmemb;
    if (is_redirect_status(context->currentStatus)) {
        return bytes;
    }

    const auto chunk = std::span{reinterpret_cast<const std::byte*>(ptr), bytes};
    if (context->observer->on_data(chunk) == detail::TransportObserver::Directive::Abort) {
        return 0;
    }
    context->activity.touch();
    return bytes;
}

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    auto& context = *static_cast<CurlContext*>(userdata);
    return guarded_callback(
        context, __func__, [&] { return write_body_impl(ptr, size, nmemb, userdata); });
}

size_t write_header_impl(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* context = static_cast<CurlContext*>(userdata);
    const std::string_view line{ptr, size * nmemb};
    if (line.starts_with("HTTP/")) {
        context->headers.clear();
        context->currentStatus = 0;
        context->currentStatus = borealis::detail::curl::status_code(line);
        context->lastBytesRead = 0;
        context->lastBytesWritten = 0;
        context->activity.start_response();
        return size * nmemb;
    }

    if (line == "\r\n") {
        if (is_redirect_status(context->currentStatus) ||
            (context->currentStatus >= 100 && context->currentStatus < 200))
        {
            return size * nmemb;
        }
        if (context->observer->on_response(context->currentStatus, std::move(context->headers)) ==
            detail::TransportObserver::Directive::Abort)
        {
            return 0;
        }
        return size * nmemb;
    }

    if (auto header = borealis::detail::curl::header(line)) {
        context->headers.emplace_back(std::move(*header));
    }
    return size * nmemb;
}

size_t write_header(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    auto& context = *static_cast<CurlContext*>(userdata);
    return guarded_callback(
        context, __func__, [&] { return write_header_impl(ptr, size, nmemb, userdata); });
}

int transfer_progress(
    void* userdata, curl_off_t, curl_off_t downloadNow, curl_off_t, curl_off_t uploadNow) {
    auto* context = static_cast<CurlContext*>(userdata);
    if (downloadNow > context->lastBytesRead) {
        context->lastBytesRead = downloadNow;
        context->activity.start_response();
    }
    if (uploadNow > context->lastBytesWritten) {
        context->lastBytesWritten = uploadNow;
        context->activity.start_response();
    }
    if (context->signals->cancelRequested.load(std::memory_order_relaxed)) {
        return 1;
    }
    if (context->deadline->expired(context->activity)) {
        context->timedOut = true;
        return 1;
    }
    return 0;
}

Error map_curl_error(CURLcode code, const CurlContext& context) {
    if (context.callbackFailed) {
        return Error::Network;
    }
    if (context.signals->cancelRequested.load(std::memory_order_relaxed)) {
        return Error::Canceled;
    }
    if (context.timedOut) {
        return Error::Timeout;
    }

    switch (code) {
    case CURLE_OK:
        return Error::None;
    case CURLE_URL_MALFORMAT:
        return Error::InvalidUrl;
    case CURLE_UNSUPPORTED_PROTOCOL:
        return Error::UnsupportedScheme;
    case CURLE_OPERATION_TIMEDOUT:
        return Error::Timeout;
    default:
        return Error::Network;
    }
}

}  // namespace

bool available() noexcept {
    return true;
}

Backend backend() noexcept {
    return Backend::LibCurl;
}

const char* backend_name() noexcept {
    return "libcurl";
}

detail::TransportResult detail::send_request(const TransportRequest& request) {
    borealis::detail::curl::initialize();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return {
            .error = Error::Network,
            .message = "Failed to create libcurl request",
        };
    }

    borealis::detail::curl::Headers headers;
    for (const Header& header : request.headers) {
        if (!headers.append(header.name + ": " + header.value)) {
            curl_easy_cleanup(curl);
            return {
                .error = Error::Network,
                .message = "Failed to allocate libcurl headers",
            };
        }
    }

    CurlContext context{
        .observer = request.observer,
        .signals = request.signals,
        .deadline = request.deadline,
    };

    const std::string url{request.url};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    const CURLcode tlsResult = curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    if (tlsResult != CURLE_OK) {
        curl_easy_cleanup(curl);
        return {
            .error = Error::Network,
            .message =
                std::string{"Failed to require TLS 1.2 or newer: "} + curl_easy_strerror(tlsResult),
        };
    }
    const auto* versionInfo = curl_version_info(CURLVERSION_NOW);
    if (versionInfo != nullptr && (versionInfo->features & CURL_VERSION_HTTP2) != 0) {
        const CURLcode httpVersionResult =
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        if (httpVersionResult != CURLE_OK) {
            curl_easy_cleanup(curl);
            return {
                .error = Error::Network,
                .message = std::string{"Failed to enable HTTP/2: "} +
                           curl_easy_strerror(httpVersionResult),
            };
        }
    }
    if (request.method == Method::Get) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (request.method == Method::Head) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.empty() ? "" : request.body.data());
        curl_easy_setopt(
            curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
        borealis::detail::curl::timeout_ms(request.deadline->connect_timeout()));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
        borealis::detail::curl::timeout_seconds(request.deadline->idle_timeout()));
    if (const auto remaining = request.deadline->remaining_total()) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, borealis::detail::curl::timeout_ms(*remaining));
    }
    if (request.allowCompression) {
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#if CURL_AT_LEAST_VERSION(7, 85, 0)
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    const Error error = map_curl_error(code, context);
    if (error == Error::None) {
        return {};
    }
    if (error == Error::Canceled) {
        return {.error = error, .message = "Request canceled"};
    }
    if (error == Error::Timeout) {
        return {.error = error, .message = "Request timed out"};
    }
    if (context.callbackFailed) {
        return {.error = error, .message = "HTTP callback failed"};
    }
    return {.error = error, .message = curl_easy_strerror(code)};
}

}  // namespace borealis::http
