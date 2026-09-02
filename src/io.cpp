#include "borealis/io.hpp"

#include "io_internal.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace borealis::io {
namespace {

bool has_scheme(std::string_view location) {
    const auto separator = location.find("://");
    return separator != std::string_view::npos && separator != 0;
}

#if defined(__ANDROID__) || defined(ANDROID)
bool is_android_uri(std::string_view location) {
    return location.starts_with("content://") || location.starts_with("file://");
}
#endif

Status filesystem_check(std::string_view location) {
    if (location.empty()) {
        return Status::NotFound;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(fs_path_from_utf8(location), error);
    if (exists) {
        return Status::Ok;
    }
    if (!error || error == std::errc::no_such_file_or_directory ||
        error == std::errc::permission_denied)
    {
        return Status::NotFound;
    }
    return Status::Failed;
}

OpenResult failed_open(Status status, std::string message) {
    return {.status = status, .message = std::move(message)};
}

std::string system_error_message(std::string_view action, int error) {
    return std::string{action} + ": " + std::generic_category().message(error);
}

}  // namespace

namespace detail {

bool safe_relative_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' ||
        (path.size() >= 2 && path[1] == ':'))
    {
        return false;
    }
    while (!path.empty()) {
        const auto separator = path.find_first_of("/\\");
        const auto segment = path.substr(0, separator);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        path.remove_prefix(separator + 1);
    }
    return true;
}

bool safe_child_name(std::string_view name) {
    return safe_relative_path(name) && name.find_first_of("/\\") == std::string_view::npos;
}

std::string fallback_display_name(std::string_view location) {
    while (location.size() > 1 && (location.back() == '/' || location.back() == '\\')) {
        location.remove_suffix(1);
    }
    const auto separator = location.find_last_of("/\\");
    return std::string{
        separator == std::string_view::npos ? location : location.substr(separator + 1)};
}

#if !defined(__APPLE__) || !TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_MACCATALYST
void release_access(void*) noexcept {}
#endif

}  // namespace detail

RandomAccessFile::~RandomAccessFile() {
    close();
}

RandomAccessFile::RandomAccessFile(RandomAccessFile&& other) noexcept
    : m_handle{std::exchange(other.m_handle,
#ifdef _WIN32
          nullptr
#else
          -1
#endif
          )},
      m_size{std::exchange(other.m_size, 0)} {
}

RandomAccessFile& RandomAccessFile::operator=(RandomAccessFile&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle,
#ifdef _WIN32
            nullptr
#else
            -1
#endif
        );
        m_size = std::exchange(other.m_size, 0);
    }
    return *this;
}

RandomAccessFile::OpenResult RandomAccessFile::open(const std::filesystem::path& path) {
    RandomAccessFile file;
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto value = static_cast<int>(GetLastError());
        return {
            .status = value == ERROR_FILE_NOT_FOUND || value == ERROR_PATH_NOT_FOUND ?
                          Status::NotFound :
                          Status::Failed,
            .message = "Failed to open file: " + std::system_category().message(value),
        };
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == FALSE) {
        const auto value = static_cast<int>(GetLastError());
        CloseHandle(handle);
        return {
            .status = Status::Failed,
            .message = "Failed to get file size: " + std::system_category().message(value),
        };
    }
    file.m_handle = handle;
    file.m_size = static_cast<uint64_t>(size.QuadPart);
#else
    const int handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle < 0) {
        const int value = errno;
        return {
            .status = value == ENOENT || value == ENOTDIR ? Status::NotFound : Status::Failed,
            .message = system_error_message("Failed to open file", value),
        };
    }
    struct stat info{};
    if (fstat(handle, &info) != 0) {
        const int value = errno;
        ::close(handle);
        return {
            .status = Status::Failed,
            .message = system_error_message("Failed to get file size", value),
        };
    }
    file.m_handle = handle;
    file.m_size = static_cast<uint64_t>(info.st_size);
#endif
    return {.status = Status::Ok, .file = std::move(file)};
}

RandomAccessFile::operator bool() const noexcept {
#ifdef _WIN32
    return m_handle != nullptr;
#else
    return m_handle >= 0;
#endif
}

size_t RandomAccessFile::read_at(
    uint64_t offset, std::span<std::byte> out, std::error_code& error) const noexcept {
    error.clear();
    size_t completed = 0;
    while (completed < out.size()) {
#ifdef _WIN32
        OVERLAPPED overlapped{};
        const uint64_t position = offset + completed;
        overlapped.Offset = static_cast<DWORD>(position);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD read = 0;
        const DWORD requested = static_cast<DWORD>(
            std::min<size_t>(out.size() - completed, std::numeric_limits<DWORD>::max()));
        if (ReadFile(static_cast<HANDLE>(m_handle), out.data() + completed, requested, &read,
                &overlapped) == FALSE)
        {
            const auto value = static_cast<int>(GetLastError());
            if (value == ERROR_HANDLE_EOF) {
                break;
            }
            error = {value, std::system_category()};
            return completed;
        }
#else
        const auto read = pread(m_handle, out.data() + completed, out.size() - completed,
            static_cast<off_t>(offset + completed));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = {errno, std::generic_category()};
            return completed;
        }
#endif
        if (read == 0) {
            break;
        }
        completed += static_cast<size_t>(read);
    }
    return completed;
}

bool RandomAccessFile::close() noexcept {
#ifdef _WIN32
    if (m_handle == nullptr) {
        return true;
    }
    const bool result = CloseHandle(static_cast<HANDLE>(m_handle)) != FALSE;
    m_handle = nullptr;
#else
    if (m_handle < 0) {
        return true;
    }
    const bool result = ::close(m_handle) == 0;
    m_handle = -1;
#endif
    m_size = 0;
    return result;
}

File::~File() {
    if (m_handle != nullptr) {
        SDL_CloseIO(m_handle);
    }
    detail::release_access(m_access);
}

File::File(File&& other) noexcept
    : m_handle{std::exchange(other.m_handle, nullptr)},
      m_access{std::exchange(other.m_access, nullptr)},
      m_writable{std::exchange(other.m_writable, false)}, m_error{std::move(other.m_error)} {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (m_handle != nullptr) {
            SDL_CloseIO(m_handle);
        }
        detail::release_access(m_access);
        m_handle = std::exchange(other.m_handle, nullptr);
        m_access = std::exchange(other.m_access, nullptr);
        m_writable = std::exchange(other.m_writable, false);
        m_error = std::move(other.m_error);
    }
    return *this;
}

void File::set_sdl_error(std::string_view action) noexcept {
    try {
        const char* error = SDL_GetError();
        m_error = std::string{action} +
                  (error != nullptr && error[0] != '\0' ? ": " + std::string{error} : " failed");
    } catch (...) {
        m_error = "I/O operation failed";
    }
}

uint64_t File::size() const noexcept {
    if (m_handle == nullptr) {
        return 0;
    }
    const Sint64 value = SDL_GetIOSize(m_handle);
    return value < 0 ? 0 : static_cast<uint64_t>(value);
}

uint64_t File::read(void* buf, uint64_t len) noexcept {
    m_error.clear();
    if (m_handle == nullptr || (buf == nullptr && len != 0)) {
        m_error = "File is not open";
        return 0;
    }
    const size_t request =
        static_cast<size_t>(std::min<uint64_t>(len, std::numeric_limits<size_t>::max()));
    const size_t read = SDL_ReadIO(m_handle, buf, request);
    if (read < request && SDL_GetIOStatus(m_handle) == SDL_IO_STATUS_ERROR) {
        set_sdl_error("Failed to read file");
    }
    return read;
}

bool File::seek(uint64_t offset) noexcept {
    m_error.clear();
    if (m_handle == nullptr || offset > static_cast<uint64_t>(std::numeric_limits<Sint64>::max())) {
        m_error = m_handle == nullptr ? "File is not open" : "File offset is too large";
        return false;
    }
    if (SDL_SeekIO(m_handle, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0) {
        set_sdl_error("Failed to seek file");
        return false;
    }
    return true;
}

bool File::write(std::span<const std::byte> bytes) noexcept {
    m_error.clear();
    if (bytes.empty()) {
        return true;
    }
    if (m_handle == nullptr || !m_writable) {
        m_error = m_handle == nullptr ? "File is not open" : "File is not writable";
        return false;
    }
    if (SDL_WriteIO(m_handle, bytes.data(), bytes.size()) != bytes.size()) {
        set_sdl_error("Failed to write file");
        return false;
    }
    return true;
}

bool File::flush() noexcept {
    m_error.clear();
    if (m_handle == nullptr) {
        return true;
    }
    if (!SDL_FlushIO(m_handle)) {
        set_sdl_error("Failed to flush file");
        return false;
    }
    return true;
}

bool File::close() noexcept {
    if (m_handle == nullptr) {
        return true;
    }
    bool success = !m_writable || flush();
    m_writable = false;
    SDL_IOStream* handle = std::exchange(m_handle, nullptr);
    if (!SDL_CloseIO(handle) && success) {
        set_sdl_error("Failed to close file");
        success = false;
    }
    detail::release_access(std::exchange(m_access, nullptr));
    return success;
}

OpenResult open(std::string_view location, File::Mode mode) {
    if (location.empty()) {
        return failed_open(Status::NotFound, "Location is empty");
    }

    std::string resolved{location};
    void* access = nullptr;
    if (location.starts_with("bookmark://")) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
        auto bookmark = detail::resolve_apple_bookmark(location, true);
        if (bookmark.status != Status::Ok) {
            return failed_open(bookmark.status, std::move(bookmark.message));
        }
        resolved = std::move(bookmark.path);
        access = bookmark.access;
#else
        return failed_open(Status::Unsupported, "Bookmarks are not supported on this platform");
#endif
    } else if (has_scheme(location)) {
#if defined(__ANDROID__) || defined(ANDROID)
        if (!is_android_uri(location)) {
            return failed_open(Status::Unsupported, "Location scheme is not supported");
        }
#else
        return failed_open(Status::Unsupported, "Location scheme is not supported");
#endif
    }

#if defined(__ANDROID__) || defined(ANDROID)
    if (mode != File::Mode::Read && resolved.starts_with("content://")) {
        auto native = detail::android_open_write(resolved, mode);
        if (native.status != Status::Ok) {
            detail::release_access(access);
            return failed_open(native.status, std::move(native.message));
        }
        return {.status = Status::Ok, .file = File{native.handle, access, true}};
    }
#endif
    if (mode != File::Mode::Read && has_scheme(resolved)) {
        detail::release_access(access);
        return failed_open(Status::Unsupported, "Write mode requires a filesystem path");
    }
    const char* openMode = mode == File::Mode::Read   ? "rb" :
                           mode == File::Mode::Append ? "ab" :
                                                        "wb";
    SDL_ClearError();
    SDL_IOStream* handle = SDL_IOFromFile(resolved.c_str(), openMode);
    if (handle == nullptr) {
        const std::string message = SDL_GetError() != nullptr && SDL_GetError()[0] != '\0' ?
                                        SDL_GetError() :
                                        "Failed to open file";
        detail::release_access(access);
        const Status status =
            check(location) == Status::NotFound ? Status::NotFound : Status::Failed;
        return failed_open(status, message);
    }
    return {.status = Status::Ok, .file = File{handle, access, mode != File::Mode::Read}};
}

Status check(std::string_view location) {
    if (location.starts_with("bookmark://")) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
        return detail::apple_bookmark_check(location);
#else
        return Status::Unsupported;
#endif
    }
#if defined(__ANDROID__) || defined(ANDROID)
    if (is_android_uri(location)) {
        return detail::android_check(location);
    }
#endif
    if (has_scheme(location)) {
        return Status::Unsupported;
    }
    return filesystem_check(location);
}

std::string display_name(std::string_view location) {
    if (location.starts_with("bookmark://")) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
        return detail::apple_bookmark_display_name(location);
#else
        return {};
#endif
    }
#if defined(__ANDROID__) || defined(ANDROID)
    if (is_android_uri(location)) {
        const auto name = detail::android_display_name(location);
        if (!name.empty()) {
            return name;
        }
    }
#endif
    return detail::fallback_display_name(location);
}

JoinResult join(std::string_view folder, std::string_view relativePath) {
    if (!detail::safe_relative_path(relativePath)) {
        return {.status = Status::Failed, .message = "Child path must be a safe relative path"};
    }
    if (folder.starts_with("bookmark://")) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
        return detail::apple_bookmark_join(folder, relativePath);
#else
        return {.status = Status::Unsupported, .message = "Bookmarks are not supported"};
#endif
    }
#if defined(__ANDROID__) || defined(ANDROID)
    if (folder.starts_with("content://")) {
        return detail::android_join(folder, relativePath);
    }
#endif
    if (has_scheme(folder)) {
        return {.status = Status::Unsupported, .message = "Folder scheme is not supported"};
    }

    const auto child = fs_path_from_utf8(folder) / fs_path_from_utf8(relativePath);
    const std::string location = fs_path_to_string(child);
    const Status status = filesystem_check(location);
    return status == Status::Ok ? JoinResult{.status = Status::Ok, .location = location} :
                                  JoinResult{.status = status, .message = "Child does not exist"};
}

JoinResult create_child(std::string_view folder, std::string_view name) {
    if (folder.empty()) {
        return {.status = Status::Failed, .message = "Folder location is empty"};
    }
    if (!detail::safe_child_name(name)) {
        return {.status = Status::Failed, .message = "Child name must be a safe file name"};
    }

    const JoinResult existing = join(folder, name);
    if (existing.status == Status::Ok) {
        return {.status = Status::AlreadyExists, .message = "Child already exists"};
    }
    if (existing.status != Status::NotFound) {
        return existing;
    }

    if (folder.starts_with("bookmark://")) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
        return detail::apple_bookmark_create_child(folder, name);
#else
        return {.status = Status::Unsupported, .message = "Bookmarks are not supported"};
#endif
    }
#if defined(__ANDROID__) || defined(ANDROID)
    if (folder.starts_with("content://")) {
        return detail::android_create_child(folder, name);
    }
#endif
    if (has_scheme(folder)) {
        return {.status = Status::Unsupported, .message = "Folder scheme is not supported"};
    }

    const auto child = fs_path_from_utf8(folder) / fs_path_from_utf8(name);
    const std::string location = fs_path_to_string(child);
    SDL_ClearError();
    SDL_IOStream* handle = SDL_IOFromFile(location.c_str(), "wbx");
    if (handle == nullptr) {
        if (filesystem_check(location) == Status::Ok) {
            return {.status = Status::AlreadyExists, .message = "Child already exists"};
        }
        const char* error = SDL_GetError();
        return {.status = Status::Failed,
            .message = error != nullptr && error[0] != '\0' ? error : "Unable to create child"};
    }
    if (!SDL_CloseIO(handle)) {
        const char* error = SDL_GetError();
        const std::string message =
            error != nullptr && error[0] != '\0' ? error : "Unable to close child";
        std::error_code ignored;
        std::filesystem::remove(child, ignored);
        return {.status = Status::Failed, .message = message};
    }
    return {.status = Status::Ok, .location = location};
}

ListResult list(std::string_view folder) {
    if (folder.starts_with("bookmark://")) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
        return detail::apple_bookmark_list(folder);
#else
        return {.status = Status::Unsupported, .message = "Bookmarks are not supported"};
#endif
    }
#if defined(__ANDROID__) || defined(ANDROID)
    if (folder.starts_with("content://")) {
        return detail::android_list(folder);
    }
#endif
    if (has_scheme(folder)) {
        return {.status = Status::Unsupported, .message = "Folder scheme is not supported"};
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator{fs_path_from_utf8(folder), error};
    if (error) {
        return {
            .status =
                error == std::errc::no_such_file_or_directory ? Status::NotFound : Status::Failed,
            .message = error.message(),
        };
    }
    ListResult result{.status = Status::Ok};
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const auto& item = *iterator;
        std::error_code typeError;
        const bool isDirectory = item.is_directory(typeError);
        if (typeError) {
            return {
                .status = Status::Failed,
                .message = typeError.message(),
            };
        }
        result.entries.push_back({
            .name = fs_path_to_string(item.path().filename()),
            .location = fs_path_to_string(item.path()),
            .isDirectory = isDirectory,
        });
        iterator.increment(error);
        if (error) {
            return {.status = Status::Failed, .message = error.message()};
        }
    }
    std::ranges::sort(result.entries, {}, &Entry::name);
    return result;
}

PathAccess::~PathAccess() {
    detail::release_access(m_access);
}

PathAccess::PathAccess(PathAccess&& other) noexcept
    : m_path{std::move(other.m_path)}, m_access{std::exchange(other.m_access, nullptr)} {
    other.m_path.clear();
}

PathAccess& PathAccess::operator=(PathAccess&& other) noexcept {
    if (this != &other) {
        detail::release_access(m_access);
        m_path = std::move(other.m_path);
        m_access = std::exchange(other.m_access, nullptr);
        other.m_path.clear();
    }
    return *this;
}

PathAccess access_path(std::string_view location) {
    if (location.starts_with("bookmark://")) {
#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
        auto resolved = detail::resolve_apple_bookmark(location, true);
        if (resolved.status == Status::Ok) {
            return {fs_path_from_utf8(resolved.path), resolved.access};
        }
#endif
        return {};
    }
    if (location.empty() || has_scheme(location)) {
        return {};
    }
    return {fs_path_from_utf8(location), nullptr};
}

bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        error = "Failed to replace file: " +
                std::system_category().message(static_cast<int>(GetLastError()));
        return false;
    }
#else
    errno = 0;
    if (rename(source.c_str(), destination.c_str()) != 0) {
        error = system_error_message("Failed to replace file", errno);
        return false;
    }
#endif
    return true;
}

}  // namespace borealis::io
