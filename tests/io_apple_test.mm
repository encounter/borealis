#include "io_internal.hpp"

#import <Foundation/Foundation.h>
#include <gtest/gtest.h>

#include <fstream>
#include <span>

namespace {

class IOSOwnedLocationTest : public testing::Test {
protected:
    void SetUp() override {
        NSString* unique =
            [[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-"
                                                               withString:@""] lowercaseString];
        hash = std::string{unique.UTF8String} + unique.UTF8String;
        directory = borealis::io::detail::apple_import_directory() / hash;
        std::filesystem::create_directories(directory);
        folder = "imported://" + hash;
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
    std::string hash;
    std::filesystem::path directory;
    std::string folder;
};

TEST_F(IOSOwnedLocationTest, SupportsReadWriteMetadataAndChildLocations) {
    using namespace borealis::io;
    const std::string name = "日本語 #%.json";
    const auto path = directory / fs_path_from_utf8(name);
    std::ofstream{path} << "before";
    const auto location = folder + "/" + name;
    EXPECT_EQ(check(location), Status::Ok);
    EXPECT_EQ(display_name(location), name);
    ASSERT_TRUE(access_path(location));
    EXPECT_EQ(access_path(location).path(), path);
    auto opened = open(location, File::Mode::Truncate);
    ASSERT_EQ(opened.status, Status::Ok) << opened.message;
    const std::string content = "after";
    ASSERT_TRUE(opened.file.write(std::as_bytes(std::span{content})));
    ASSERT_TRUE(opened.file.close());
    opened = open(location, File::Mode::Append);
    ASSERT_EQ(opened.status, Status::Ok) << opened.message;
    ASSERT_TRUE(opened.file.write(std::as_bytes(std::span{content})));
    ASSERT_TRUE(opened.file.close());
    opened = open(location);
    ASSERT_EQ(opened.status, Status::Ok) << opened.message;
    EXPECT_EQ(opened.file.size(), 10u);
    char bytes[10]{};
    EXPECT_EQ(opened.file.read(bytes, sizeof(bytes)), sizeof(bytes));
    EXPECT_EQ(std::string_view(bytes, sizeof(bytes)), "afterafter");
    EXPECT_EQ(join(folder, name).location, location);
    auto created = create_child(folder, "new.txt");
    ASSERT_EQ(created.status, Status::Ok) << created.message;
    EXPECT_EQ(created.location, folder + "/new.txt");
    EXPECT_EQ(create_child(folder, "new.txt").status, Status::AlreadyExists);
    auto listed = list(folder);
    ASSERT_EQ(listed.status, Status::Ok) << listed.message;
    ASSERT_EQ(listed.entries.size(), 2u);
    for (const auto& entry : listed.entries) {
        EXPECT_TRUE(entry.location.starts_with(folder + "/"));
        EXPECT_EQ(check(entry.location), Status::Ok);
    }
    EXPECT_EQ(check(folder + "/absent"), Status::NotFound);
}

TEST_F(IOSOwnedLocationTest, RejectsEscapingMalformedAndTruncatedLocations) {
    using namespace borealis::io;
    std::vector<std::string> invalid{
        "imported://",
        "imported://../outside",
        "imported://short/name",
        folder + "/../outside",
        folder + "/nested/../../outside",
        folder + "/\\outside",
        folder + "/./name",
        folder + "//name",
    };
    invalid.push_back(folder + "/name" + std::string{"\0", 1} + "/outside");
    for (const auto& location : invalid) {
        EXPECT_FALSE(access_path(location)) << location;
        EXPECT_NE(open(location).status, Status::Ok) << location;
        EXPECT_NE(check(location), Status::Ok) << location;
    }
}

}  // namespace
