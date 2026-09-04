#pragma once

#include "../ascii.hpp"

#include <string_view>

namespace borealis::detail::headers {

inline std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                                 value.back() == '\n'))
    {
        value.remove_suffix(1);
    }
    return value;
}

}  // namespace borealis::detail::headers
