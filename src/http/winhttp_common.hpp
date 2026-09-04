#pragma once

#include "borealis/http.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace borealis::detail::winhttp {

class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET value) : m_handle{value} {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    ~Handle() {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
        }
    }

    HINTERNET get() const noexcept { return m_handle; }
    HINTERNET release() noexcept { return std::exchange(m_handle, nullptr); }
    operator HINTERNET() const { return m_handle; }

private:
    HINTERNET m_handle = nullptr;
};

inline bool configure_secure_protocols(HINTERNET session) {
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        return true;
    }
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#endif
    return WinHttpSetOption(
               session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)) != FALSE;
}

inline std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty() || value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
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

inline std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
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

inline DWORD timeout_ms(std::chrono::milliseconds timeout) {
    return static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(
        timeout.count(), 1, std::numeric_limits<int>::max()));
}

struct CrackedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

inline std::optional<CrackedUrl> crack_url(std::wstring_view url) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &components)) {
        return std::nullopt;
    }
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && components.nScheme != INTERNET_SCHEME_HTTP) {
        SetLastError(ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
        return std::nullopt;
    }

    CrackedUrl result{
        .host = std::wstring{components.lpszHostName, components.dwHostNameLength},
        .port = components.nPort,
        .secure = secure,
    };
    if (components.lpszUrlPath != nullptr && components.dwUrlPathLength != 0) {
        result.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength != 0) {
        result.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (result.path.empty() || result.path.front() != L'/') {
        result.path.insert(result.path.begin(), L'/');
    }
    return result;
}

inline std::string error_message(DWORD error, std::string_view operation) {
    return std::string{operation} + " (" + std::to_string(error) + ")";
}

template <typename Error>
Error map_error(DWORD error) {
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

inline void parse_headers(std::wstring_view raw, std::vector<http::Header>& headers) {
    size_t start = 0;
    bool first = true;
    while (start < raw.size()) {
        size_t end = raw.find(L"\r\n", start);
        if (end == std::wstring_view::npos) {
            end = raw.size();
        }
        const std::wstring_view line = raw.substr(start, end - start);
        if (!first) {
            const size_t colon = line.find(L':');
            if (colon != std::wstring_view::npos) {
                std::wstring_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) {
                    value.remove_prefix(1);
                }
                while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
                    value.remove_suffix(1);
                }
                headers.push_back({
                    .name = wide_to_utf8(line.substr(0, colon)),
                    .value = wide_to_utf8(value),
                });
            }
        }
        first = false;
        if (end == raw.size()) {
            break;
        }
        start = end + 2;
    }
}

inline bool query_response(HINTERNET request, int& status, std::vector<http::Header>& headers) {
    DWORD code = 0;
    DWORD codeBytes = sizeof(code);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeBytes, WINHTTP_NO_HEADER_INDEX))
    {
        return false;
    }
    status = static_cast<int>(code);
    DWORD rawBytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER, &rawBytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    std::wstring raw(rawBytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
            raw.data(), &rawBytes, WINHTTP_NO_HEADER_INDEX))
    {
        return false;
    }
    if (!raw.empty() && raw.back() == L'\0') {
        raw.pop_back();
    }
    parse_headers(raw, headers);
    return true;
}

}  // namespace borealis::detail::winhttp
