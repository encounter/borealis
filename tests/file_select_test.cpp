#include "file_select/file_select_internal.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class FileSelectTest : public testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() / "borealis_file_select_test";
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::filesystem::create_directories(directory);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    static std::string read(const std::filesystem::path& path) {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    std::filesystem::path directory;
};

TEST_F(FileSelectTest, AtomicExportReplacesDestinationAndRemovesStage) {
    const auto source = directory / "source.txt";
    const auto destination = directory / "destination.txt";
    std::ofstream{source, std::ios::binary} << "new contents";
    std::ofstream{destination, std::ios::binary} << "old contents";

    const auto result = borealis::file_select::detail::copy_export_file(
        source.string(), destination.string(), true);
    ASSERT_EQ(result.status, borealis::file_select::Status::Selected) << result.message;
    EXPECT_EQ(read(destination), "new contents");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{directory},
                  std::filesystem::directory_iterator{}),
        2);
}

TEST_F(FileSelectTest, FailedAtomicExportPreservesDestination) {
    const auto destination = directory / "destination.txt";
    std::ofstream{destination, std::ios::binary} << "old contents";

    const auto result = borealis::file_select::detail::copy_export_file(
        (directory / "missing.txt").string(), destination.string(), true);
    EXPECT_EQ(result.status, borealis::file_select::Status::Failed);
    EXPECT_EQ(read(destination), "old contents");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{directory},
                  std::filesystem::directory_iterator{}),
        1);
}

}  // namespace
