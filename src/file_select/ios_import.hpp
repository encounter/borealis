#pragma once

#include "borealis/io.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace borealis::file_select::detail {

enum class IOSIdentity { Match, Mismatch, Unknown };

inline IOSIdentity compare_ios_identity(
    std::string_view applicationIdentifier, std::string_view bundleIdentifier) {
    const auto dot = applicationIdentifier.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == applicationIdentifier.size() ||
        bundleIdentifier.empty() || applicationIdentifier.find('*') != std::string_view::npos ||
        applicationIdentifier.find('\0') != std::string_view::npos ||
        bundleIdentifier.find('\0') != std::string_view::npos)
    {
        return IOSIdentity::Unknown;
    }
    return applicationIdentifier.substr(dot + 1) == bundleIdentifier ? IOSIdentity::Match :
                                                                       IOSIdentity::Mismatch;
}

struct IOSImportBatch {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> files;

    IOSImportBatch() = default;
    IOSImportBatch(const IOSImportBatch&) = delete;
    IOSImportBatch& operator=(const IOSImportBatch&) = delete;
    ~IOSImportBatch();
};

std::shared_ptr<IOSImportBatch> stage_ios_imports(
    void* urls, const std::filesystem::path& root, std::string& error);
io::JoinResult retain_ios_import(
    const std::filesystem::path& source, const std::filesystem::path& root);

}  // namespace borealis::file_select::detail
