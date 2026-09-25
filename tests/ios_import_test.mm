#include "file_select/ios_import.hpp"

#import <Foundation/Foundation.h>
#include <gtest/gtest.h>

#include <fstream>

namespace {

class IOSImportTest : public testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() / NSUUID.UUID.UUIDString.UTF8String;
        root = directory / "imported";
        std::filesystem::create_directories(directory / "picker");
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
    std::filesystem::path file(std::string_view name, std::string_view contents) {
        auto path = directory / "picker" / borealis::io::fs_path_from_utf8(name);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream{path, std::ios::binary} << contents;
        return path;
    }
    std::shared_ptr<borealis::file_select::detail::IOSImportBatch> stage(
        const std::vector<std::filesystem::path>& files, std::string& error) {
        NSMutableArray<NSURL*>* urls = [NSMutableArray new];
        for (const auto& path : files) {
            [urls addObject:[NSURL fileURLWithPath:@(path.c_str())]];
        }
        return borealis::file_select::detail::stage_ios_imports((__bridge void*)urls, root, error);
    }
    std::filesystem::path retained_path(const std::string& location) {
        return root / location.substr(std::string_view{"imported://"}.size());
    }
    std::filesystem::path directory;
    std::filesystem::path root;
};

TEST_F(IOSImportTest, RetainsUnicodeNamesAndDeduplicatesContents) {
    std::string error;
    const auto source = file("日本語.iso", "abc");
    auto first = stage({source}, error);
    ASSERT_TRUE(first) << error;
    EXPECT_FALSE(std::filesystem::exists(source));
    auto result = borealis::file_select::detail::retain_ios_import(first->files[0], root);
    ASSERT_EQ(result.status, borealis::io::Status::Ok) << result.message;
    EXPECT_EQ(result.location,
        "imported://ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad/日本語.iso");
    const auto owned = retained_path(result.location);
    const auto stagedDirectory = first->directory;
    first.reset();
    EXPECT_FALSE(std::filesystem::exists(stagedDirectory));
    EXPECT_EQ(std::filesystem::file_size(owned), 3u);

    auto second = stage({file("日本語.iso", "abc")}, error);
    ASSERT_TRUE(second) << error;
    auto duplicate = borealis::file_select::detail::retain_ios_import(second->files[0], root);
    EXPECT_EQ(duplicate.status, borealis::io::Status::Ok) << duplicate.message;
    EXPECT_EQ(duplicate.location, result.location);

    std::ofstream{owned} << "edited by caller";
    auto changed = borealis::file_select::detail::retain_ios_import(second->files[0], root);
    ASSERT_EQ(changed.status, borealis::io::Status::Ok) << changed.message;
    EXPECT_NE(changed.location, result.location);
    EXPECT_EQ(std::filesystem::file_size(owned), 16u);
    EXPECT_EQ(std::filesystem::file_size(retained_path(changed.location)), 3u);
}

TEST_F(IOSImportTest, StagesAllFilesWithIdenticalNamesBeforeRetaining) {
    std::string error;
    auto batch = stage({file("a/mod.json", "one"), file("b/mod.json", "two")}, error);
    ASSERT_TRUE(batch) << error;
    ASSERT_EQ(batch->files.size(), 2u);
    auto first = borealis::file_select::detail::retain_ios_import(batch->files[0], root);
    auto second = borealis::file_select::detail::retain_ios_import(batch->files[1], root);
    ASSERT_EQ(first.status, borealis::io::Status::Ok) << first.message;
    ASSERT_EQ(second.status, borealis::io::Status::Ok) << second.message;
    EXPECT_NE(first.location, second.location);
    EXPECT_EQ(retained_path(first.location).filename(), "mod.json");
    EXPECT_EQ(retained_path(second.location).filename(), "mod.json");
}

TEST_F(IOSImportTest, FailuresRemoveUnpublishedStagingAndKeepExistingImports) {
    std::string error;
    auto initial = stage({file("keep.txt", "keep")}, error);
    ASSERT_TRUE(initial) << error;
    auto retained = borealis::file_select::detail::retain_ios_import(initial->files[0], root);
    ASSERT_EQ(retained.status, borealis::io::Status::Ok) << retained.message;
    initial.reset();
    EXPECT_FALSE(stage({file("first.txt", "first"), directory / "missing"}, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(std::filesystem::exists(retained_path(retained.location)));
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        EXPECT_FALSE(entry.path().filename().string().starts_with(".pending-"));
    }
    auto pending = stage({file("failure.txt", "value")}, error);
    ASSERT_TRUE(pending) << error;
    const auto blocked = file("blocked", "file, not directory");
    auto failure = borealis::file_select::detail::retain_ios_import(pending->files[0], blocked);
    EXPECT_EQ(failure.status, borealis::io::Status::Failed);
    EXPECT_TRUE(std::filesystem::exists(pending->files[0]));
}

TEST_F(IOSImportTest, RetainsEmptyFiles) {
    std::string error;
    auto batch = stage({file("empty", "")}, error);
    ASSERT_TRUE(batch) << error;
    auto result = borealis::file_select::detail::retain_ios_import(batch->files[0], root);
    ASSERT_EQ(result.status, borealis::io::Status::Ok) << result.message;
    EXPECT_EQ(std::filesystem::file_size(retained_path(result.location)), 0u);
}

}  // namespace
