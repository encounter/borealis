#pragma once

#include "borealis/io.hpp"

#include <string>
#include <string_view>

namespace borealis::io::detail {

constexpr std::string_view AppleBookmarkPrefix = "bookmark://";
constexpr std::string_view AppleImportPrefix = "imported://";

inline bool is_apple_location(std::string_view location) {
    return location.starts_with(AppleBookmarkPrefix) || location.starts_with(AppleImportPrefix);
}

struct NativeOpenResult {
    Status status = Status::Failed;
    SDL_IOStream* handle = nullptr;
    std::string message;
};

struct ResolvedPath {
    Status status = Status::Failed;
    std::string path;
    void* access = nullptr;
    std::string message;
};

void release_access(void* access) noexcept;

#if defined(__APPLE__)
std::filesystem::path apple_import_directory();
ResolvedPath resolve_apple_location(std::string_view location, bool startAccess);
std::string apple_bookmark_for_url(void* url, std::string& error);
std::string apple_bookmark_for_path(std::string_view path, std::string& error);
std::string apple_location_display_name(std::string_view location);
Status apple_location_check(std::string_view location);
JoinResult apple_location_join(std::string_view folder, std::string_view relativePath);
JoinResult apple_location_create_child(std::string_view folder, std::string_view name);
ListResult apple_location_list(std::string_view folder);
#endif

#if defined(__ANDROID__) || defined(ANDROID)
Status android_check(std::string_view location);
std::string android_display_name(std::string_view location);
NativeOpenResult android_open_write(std::string_view location, File::Mode mode);
JoinResult android_join(std::string_view folder, std::string_view relativePath);
JoinResult android_create_child(std::string_view folder, std::string_view name);
ListResult android_list(std::string_view folder);
#endif

bool safe_relative_path(std::string_view path);
bool safe_child_name(std::string_view name);
std::string fallback_display_name(std::string_view location);

}  // namespace borealis::io::detail
