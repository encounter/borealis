#include "borealis/http.hpp"

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

Result request(const Request&) {
    return {
        .error = Error::NoBackend,
        .message = "No HTTP backend is available",
    };
}

Result get(const Request& originalRequest) {
    Request getRequest = originalRequest;
    getRequest.method = Method::Get;
    getRequest.body.clear();
    return request(getRequest);
}

}  // namespace borealis::http
