#include "borealis/http.hpp"

#include "../http_internal.hpp"

namespace borealis::http {

bool available() noexcept {
    return false;
}

Backend backend() noexcept {
    return Backend::None;
}

const char* backend_name() noexcept {
    return "none";
}

detail::TransportResult detail::send_request(const TransportRequest&) {
    return {
        .error = Error::NoBackend,
        .message = "No HTTP backend is available",
    };
}

}  // namespace borealis::http
