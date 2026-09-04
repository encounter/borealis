#include "borealis/url.hpp"

#include "ascii.hpp"

#include <algorithm>
#include <charconv>
#include <ranges>

namespace borealis::url {
namespace {

bool valid_scheme(std::string_view scheme) {
    if (scheme.empty() || !((scheme.front() >= 'A' && scheme.front() <= 'Z') ||
                              (scheme.front() >= 'a' && scheme.front() <= 'z')))
    {
        return false;
    }
    return std::ranges::all_of(scheme.substr(1), [](char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '+' || value == '-' || value == '.';
    });
}

}  // namespace

std::optional<Parsed> parse(std::string_view text) {
    const size_t separator = text.find("://");
    if (separator == std::string_view::npos || !valid_scheme(text.substr(0, separator)) ||
        separator + 3 >= text.size() || text.find('#') != std::string_view::npos ||
        text.find('\\') != std::string_view::npos ||
        std::ranges::any_of(text, [](unsigned char value) { return value <= 32 || value == 127; }))
    {
        return std::nullopt;
    }

    Parsed result{.scheme = std::string{text.substr(0, separator)}};
    std::ranges::transform(result.scheme, result.scheme.begin(), detail::ascii_lower);

    const size_t authorityStart = separator + 3;
    const size_t authorityEnd = text.find_first_of("/?", authorityStart);
    result.hasResource = authorityEnd != std::string_view::npos;
    const std::string_view authority = text.substr(authorityStart, authorityEnd - authorityStart);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view host;
    std::string_view port;
    bool portSpecified = false;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1 ||
            (close + 1 != authority.size() && authority[close + 1] != ':'))
        {
            return std::nullopt;
        }
        result.ipv6 = true;
        host = authority.substr(1, close - 1);
        if (close + 1 != authority.size()) {
            portSpecified = true;
            port = authority.substr(close + 2);
        }
    } else {
        if (authority.find_first_of("[]") != std::string_view::npos) {
            return std::nullopt;
        }
        const size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos && authority.find(':') != colon) {
            return std::nullopt;
        }
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos) {
            portSpecified = true;
            port = authority.substr(colon + 1);
        }
    }
    if (host.empty() || (portSpecified && port.empty())) {
        return std::nullopt;
    }

    result.host = host;
    std::ranges::transform(result.host, result.host.begin(), detail::ascii_lower);
    if (portSpecified) {
        unsigned int portNumber = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), portNumber);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
            portNumber > 65535)
        {
            return std::nullopt;
        }
        result.port = static_cast<uint16_t>(portNumber);
    }
    return result;
}

}  // namespace borealis::url
