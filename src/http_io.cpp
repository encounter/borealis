#include "http_io.hpp"

#include <cerrno>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace borealis::http::detail::io {
namespace {

std::string system_error_message(std::string_view action, int error) {
    return std::string{action} + ": " + std::generic_category().message(error);
}

}  // namespace

File::~File() {
    if (m_file != nullptr) {
        std::fclose(m_file);
    }
}

bool File::open(const std::filesystem::path& path, Mode mode, std::string& error) {
    if (m_file != nullptr) {
        error = "File is already open";
        return false;
    }

#ifdef _WIN32
    const wchar_t* openMode = mode == Mode::Append ? L"ab" : L"wb";
    const errno_t openError = _wfopen_s(&m_file, path.c_str(), openMode);
    if (openError != 0 || m_file == nullptr) {
        error = system_error_message("Failed to open file", openError != 0 ? openError : errno);
        return false;
    }
#else
    const char* openMode = mode == Mode::Append ? "ab" : "wb";
    errno = 0;
    m_file = std::fopen(path.c_str(), openMode);
    if (m_file == nullptr) {
        error = system_error_message("Failed to open file", errno);
        return false;
    }
#endif
    return true;
}

bool File::write(std::span<const std::byte> bytes, std::string& error) {
    if (bytes.empty()) {
        return true;
    }
    if (m_file == nullptr) {
        error = "File is not open";
        return false;
    }
    errno = 0;
    if (std::fwrite(bytes.data(), 1, bytes.size(), m_file) != bytes.size()) {
        error = system_error_message("Failed to write file", errno != 0 ? errno : EIO);
        return false;
    }
    return true;
}

bool File::flush(std::string& error) {
    if (m_file == nullptr) {
        return true;
    }
    errno = 0;
    if (std::fflush(m_file) != 0) {
        error = system_error_message("Failed to flush file", errno != 0 ? errno : EIO);
        return false;
    }
    return true;
}

bool File::close(std::string& error) {
    if (m_file == nullptr) {
        return true;
    }

    bool success = flush(error);
    std::FILE* file = m_file;
    m_file = nullptr;
    errno = 0;
    if (std::fclose(file) != 0 && success) {
        error = system_error_message("Failed to close file", errno != 0 ? errno : EIO);
        success = false;
    }
    return success;
}

bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        const DWORD moveError = GetLastError();
        error = "Failed to replace file: " +
                std::system_category().message(static_cast<int>(moveError));
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

}  // namespace borealis::http::detail::io
