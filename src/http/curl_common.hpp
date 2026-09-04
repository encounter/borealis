#pragma once

#include "borealis/http.hpp"
#include "header_common.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace borealis::detail::curl {

class Headers {
public:
    Headers() = default;
    Headers(const Headers&) = delete;
    Headers& operator=(const Headers&) = delete;

    ~Headers() {
        if (m_list != nullptr) {
            curl_slist_free_all(m_list);
        }
    }

    bool append(const std::string& header) {
        curl_slist* next = curl_slist_append(m_list, header.c_str());
        if (next == nullptr) {
            return false;
        }
        m_list = next;
        return true;
    }

    curl_slist* get() const { return m_list; }

private:
    curl_slist* m_list = nullptr;
};

inline void initialize() {
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

inline long timeout_ms(std::chrono::milliseconds timeout) {
    return static_cast<long>(std::clamp<std::chrono::milliseconds::rep>(
        timeout.count(), 1, std::numeric_limits<long>::max()));
}

inline long timeout_seconds(std::chrono::milliseconds timeout) {
    const auto milliseconds = std::max<std::chrono::milliseconds::rep>(1, timeout.count());
    const auto seconds = milliseconds / 1000 + (milliseconds % 1000 != 0 ? 1 : 0);
    return static_cast<long>(
        std::min<std::chrono::milliseconds::rep>(seconds, std::numeric_limits<long>::max()));
}

inline int status_code(std::string_view line) {
    int result = 0;
    const size_t start = line.find(' ');
    if (start != std::string_view::npos) {
        std::from_chars(line.data() + start + 1, line.data() + line.size(), result);
    }
    return result;
}

inline std::optional<http::Header> header(std::string_view line) {
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    return http::Header{
        .name = std::string{line.substr(0, colon)},
        .value = std::string{headers::trim(line.substr(colon + 1))},
    };
}

}  // namespace borealis::detail::curl
