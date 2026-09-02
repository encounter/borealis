#include "borealis/http.hpp"

#include "http_internal.hpp"

#include <exception>
#include <utility>

namespace borealis::http {
namespace detail {

Result validate_request(const Request& request) {
    if (request.url.empty()) {
        return {
            .error = Error::InvalidUrl,
            .message = "URL is empty",
        };
    }
    if (!request.url.starts_with("https://")) {
        return {
            .error = Error::UnsupportedScheme,
            .message = "Only https:// URLs are supported",
        };
    }
    return {};
}

}  // namespace detail

Task<Result> start(Request request) {
    if (!available()) {
        return borealis::detail::make_ready_task(Result{
            .error = Error::NoBackend,
            .message = "No HTTP backend is available",
        });
    }

    if (Result error = detail::validate_request(request); error.error != Error::None) {
        return borealis::detail::make_ready_task(std::move(error));
    }

    borealis::detail::TaskSource<Result> source;
    Task<Result> task = source.task();
    auto signals = source.signals();
    const bool submitted = borealis::detail::submit_task(
        [request = std::move(request), source](borealis::detail::TaskSignals* taskSignals) mutable {
            Result result;
            if (taskSignals->cancelRequested.load(std::memory_order_relaxed)) {
                result = {.error = Error::Canceled, .message = "Request canceled"};
            } else {
                try {
                    result = detail::perform(request, taskSignals);
                } catch (const std::exception& exception) {
                    result = {.error = Error::Network, .message = exception.what()};
                } catch (...) {
                    result = {.error = Error::Network, .message = "HTTP backend failed"};
                }
            }
            source.set_value(std::move(result));
        },
        std::move(signals));
    if (!submitted) {
        source.set_value({
            .error = Error::Network,
            .message = "HTTP worker pool could not accept the request",
        });
    }
    return task;
}

}  // namespace borealis::http
