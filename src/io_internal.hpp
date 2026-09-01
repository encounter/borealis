#pragma once

#include "borealis/io.hpp"

#include <string>
#include <string_view>

namespace borealis::io::detail {

struct ResolvedPath {
    Status status = Status::Failed;
    std::string path;
    void* access = nullptr;
    std::string message;
};

void release_access(void* access) noexcept;

#if defined(__APPLE__)
ResolvedPath resolve_apple_bookmark(std::string_view location, bool startAccess);
std::string apple_bookmark_for_url(void* url, std::string& error);
std::string apple_bookmark_for_path(std::string_view path, std::string& error);
std::string apple_bookmark_display_name(std::string_view location);
Status apple_bookmark_check(std::string_view location);
JoinResult apple_bookmark_join(std::string_view folder, std::string_view relativePath);
ListResult apple_bookmark_list(std::string_view folder);
#endif

#if defined(__ANDROID__) || defined(ANDROID)
Status android_check(std::string_view location);
std::string android_display_name(std::string_view location);
JoinResult android_join(std::string_view folder, std::string_view relativePath);
ListResult android_list(std::string_view folder);
#endif

bool safe_relative_path(std::string_view path);
std::string fallback_display_name(std::string_view location);

}  // namespace borealis::io::detail
