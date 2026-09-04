#include "../ws_internal.hpp"

namespace borealis::ws::detail {

std::unique_ptr<Transport> make_transport() {
    return {};
}

bool backend_available() noexcept {
    return false;
}

}  // namespace borealis::ws::detail
