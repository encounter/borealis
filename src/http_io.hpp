#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>

namespace borealis::http::detail::io {

class File {
public:
    enum class Mode {
        Truncate,
        Append,
    };

    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool open(const std::filesystem::path& path, Mode mode, std::string& error);
    bool write(std::span<const std::byte> bytes, std::string& error);
    bool flush(std::string& error);
    bool close(std::string& error);
    bool is_open() const noexcept { return m_file != nullptr; }

private:
    std::FILE* m_file = nullptr;
};

bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error);

}  // namespace borealis::http::detail::io
