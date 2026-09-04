#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace borealis::url {

struct Parsed {
    std::string scheme;
    std::string host;
    std::optional<uint16_t> port;
    bool ipv6 = false;
    bool hasResource = false;
};

std::optional<Parsed> parse(std::string_view text);

}  // namespace borealis::url
