#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SDL_IOStream;

namespace borealis::io {

enum class Status {
    Ok,
    NotFound,
    Unsupported,
    Failed,
    AlreadyExists,
};

struct OpenResult;

class File {
public:
    enum class Mode {
        Read,
        Truncate,
        Append,
    };

    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    explicit operator bool() const noexcept { return m_handle != nullptr; }
    uint64_t size() const noexcept;
    uint64_t read(void* buf, uint64_t len) noexcept;
    bool seek(uint64_t offset) noexcept;
    bool write(std::span<const std::byte> bytes) noexcept;
    bool flush() noexcept;
    bool close() noexcept;
    bool writable() const noexcept { return m_writable; }
    const std::string& error() const noexcept { return m_error; }
    SDL_IOStream* handle() const noexcept { return m_handle; }

private:
    friend struct OpenResult;
    friend OpenResult open(std::string_view location, Mode mode);

    File(SDL_IOStream* handle, void* access, bool writable) noexcept
        : m_handle{handle}, m_access{access}, m_writable{writable} {}
    void set_sdl_error(std::string_view action) noexcept;

    SDL_IOStream* m_handle = nullptr;
    void* m_access = nullptr;
    bool m_writable = false;
    std::string m_error;
};

/** Read-only file whose handle remains bound across rename and delete. */
class RandomAccessFile {
public:
    struct OpenResult;

    RandomAccessFile() = default;
    ~RandomAccessFile();
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;
    RandomAccessFile(RandomAccessFile&& other) noexcept;
    RandomAccessFile& operator=(RandomAccessFile&& other) noexcept;

    static OpenResult open(const std::filesystem::path& path);

    explicit operator bool() const noexcept;
    uint64_t size() const noexcept { return m_size; }
    /** Reads from an absolute offset without changing shared cursor state. */
    size_t read_at(
        uint64_t offset, std::span<std::byte> out, std::error_code& error) const noexcept;
    bool close() noexcept;

private:
#ifdef _WIN32
    void* m_handle = nullptr;
#else
    int m_handle = -1;
#endif
    uint64_t m_size = 0;
};

struct RandomAccessFile::OpenResult {
    Status status = Status::Failed;
    RandomAccessFile file;
    std::string message;
};

struct OpenResult {
    Status status = Status::Failed;
    File file;
    std::string message;
};

/** Opens a file from an opaque location. */
OpenResult open(std::string_view location, File::Mode mode = File::Mode::Read);

/** File access probe. */
Status check(std::string_view location);

/** Returns a short human-readable name for an opaque location. */
std::string display_name(std::string_view location);

bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error);

struct JoinResult {
    Status status = Status::Failed;
    std::string location;
    std::string message;
};

/** Resolves an existing child in a folder. */
JoinResult join(std::string_view folder, std::string_view relativePath);

/** Creates an empty file in a folder without replacing an existing child. */
JoinResult create_child(std::string_view folder, std::string_view name);

struct Entry {
    std::string name;
    std::string location;
    bool isDirectory = false;
};

struct ListResult {
    Status status = Status::Failed;
    std::vector<Entry> entries;
    std::string message;
};

ListResult list(std::string_view folder);

class PathAccess {
public:
    PathAccess() = default;
    ~PathAccess();
    PathAccess(const PathAccess&) = delete;
    PathAccess& operator=(const PathAccess&) = delete;
    PathAccess(PathAccess&& other) noexcept;
    PathAccess& operator=(PathAccess&& other) noexcept;

    explicit operator bool() const noexcept { return !m_path.empty(); }
    const std::filesystem::path& path() const noexcept { return m_path; }

private:
    friend PathAccess access_path(std::string_view location);
    PathAccess(std::filesystem::path path, void* access)
        : m_path{std::move(path)}, m_access{access} {}

    std::filesystem::path m_path;
    void* m_access = nullptr;
};

/** Holds any platform access grant while providing a filesystem-backed location. */
PathAccess access_path(std::string_view location);

/** Converts a filesystem path to UTF-8 without using the Windows ANSI code page. */
inline std::string fs_path_to_string(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.string();
#endif
}

/** Converts a filesystem path to UTF-8 with '/' separators. */
inline std::string fs_path_to_generic_string(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::u8string u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.generic_string();
#endif
}

/** Constructs a filesystem path from UTF-8. */
inline std::filesystem::path fs_path_from_utf8(std::string_view utf8) {
#if defined(_WIN32)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

}  // namespace borealis::io
