#include "borealis/io.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class IOTest : public testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() / "borealis_io_test";
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::filesystem::create_directories(directory / "nested");
        std::ofstream{directory / "sample.txt", std::ios::binary} << "abcdef";
        std::ofstream{directory / "nested" / "child.txt", std::ios::binary} << "child";
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path directory;
};

TEST_F(IOTest, StreamsAndSeeks) {
    const auto location = borealis::io::fs_path_to_string(directory / "sample.txt");
    auto opened = borealis::io::open(location);
    ASSERT_EQ(opened.status, borealis::io::Status::Ok) << opened.message;
    EXPECT_EQ(opened.file.size(), 6u);

    std::array<char, 4> bytes{};
    EXPECT_EQ(opened.file.read(bytes.data(), 3), 3u);
    EXPECT_EQ(std::string_view(bytes.data(), 3), "abc");
    ASSERT_TRUE(opened.file.seek(2));
    EXPECT_EQ(opened.file.read(bytes.data(), 4), 4u);
    EXPECT_EQ(std::string_view(bytes.data(), 4), "cdef");
    EXPECT_TRUE(opened.file.close());
}

TEST_F(IOTest, ChecksListsAndJoins) {
    const auto folder = borealis::io::fs_path_to_string(directory);
    EXPECT_EQ(borealis::io::check(folder), borealis::io::Status::Ok);
    EXPECT_EQ(borealis::io::check(folder + "/missing"), borealis::io::Status::NotFound);

    const auto joined = borealis::io::join(folder, "nested/child.txt");
    ASSERT_EQ(joined.status, borealis::io::Status::Ok) << joined.message;
    EXPECT_EQ(borealis::io::display_name(joined.location), "child.txt");
    EXPECT_NE(borealis::io::join(folder, "../sample.txt").status, borealis::io::Status::Ok);

    const auto listed = borealis::io::list(folder);
    ASSERT_EQ(listed.status, borealis::io::Status::Ok) << listed.message;
    ASSERT_EQ(listed.entries.size(), 2u);
    EXPECT_EQ(listed.entries[0].name, "nested");
    EXPECT_TRUE(listed.entries[0].isDirectory);
    EXPECT_EQ(listed.entries[1].name, "sample.txt");
    EXPECT_FALSE(listed.entries[1].isDirectory);
}

}  // namespace
