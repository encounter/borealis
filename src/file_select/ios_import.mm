#include "ios_import.hpp"

#include "../io_internal.hpp"

#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

#include <array>
#include <fstream>

namespace borealis::file_select::detail {
namespace {

NSString* ns_path(const std::filesystem::path& path) {
    const std::string utf8 = io::fs_path_to_string(path);
    return [[NSString alloc] initWithBytes:utf8.data()
                                    length:utf8.size()
                                  encoding:NSUTF8StringEncoding];
}

std::string error_message(NSError* error, const char* fallback) {
    return error.localizedDescription.UTF8String ?: fallback;
}

std::string hex_digest(const unsigned char* digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(CC_SHA256_DIGEST_LENGTH * 2);
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 15]);
    }
    return result;
}

std::string file_digest(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    CC_SHA256_CTX context{};
    CC_SHA256_Init(&context);
    auto buffer = std::make_unique<std::array<char, 1024 * 1024>>();
    while (input) {
        input.read(buffer->data(), static_cast<std::streamsize>(buffer->size()));
        CC_SHA256_Update(&context, buffer->data(), static_cast<CC_LONG>(input.gcount()));
    }
    if (!input.eof() || input.bad()) {
        return {};
    }
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &context);
    return hex_digest(digest.data());
}

std::string unique_digest() {
    NSData* data = [NSUUID.UUID.UUIDString dataUsingEncoding:NSUTF8StringEncoding];
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(data.bytes, static_cast<CC_LONG>(data.length), digest.data());
    return hex_digest(digest.data());
}

}  // namespace

IOSImportBatch::~IOSImportBatch() {
    if (!directory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
}

std::shared_ptr<IOSImportBatch> stage_ios_imports(
    void* opaqueUrls, const std::filesystem::path& root, std::string& error) {
    NSArray<NSURL*>* urls = (__bridge NSArray<NSURL*>*)opaqueUrls;
    if (root.empty() || urls.count == 0) {
        error = "No files or import storage available";
        return {};
    }
    auto batch = std::make_shared<IOSImportBatch>();
    batch->directory = root / (".pending-" + std::string{NSUUID.UUID.UUIDString.UTF8String});
    NSFileManager* manager = NSFileManager.defaultManager;
    for (NSURL* url in urls) {
        const char* name = url.lastPathComponent.UTF8String;
        NSNumber* regular = nil;
        NSError* stageError = nil;
        if (!url.isFileURL || name == nullptr || !io::detail::safe_child_name(name) ||
            ![url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:&stageError] ||
            !regular.boolValue)
        {
            error = error_message(stageError, "Only regular files can be imported");
            return {};
        }
        // Separate subdirectories also support multi-selection of identical names.
        const auto directory = batch->directory / std::to_string(batch->files.size());
        const auto destination = directory / io::fs_path_from_utf8(name);
        if (![manager createDirectoryAtPath:ns_path(directory)
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:&stageError])
        {
            error = error_message(stageError, "Unable to create import storage");
            return {};
        }
        NSURL* destinationUrl = [NSURL fileURLWithPath:ns_path(destination)];
        NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        __block NSError* moveError = nil;
        __block BOOL moved = NO;
        [coordinator coordinateWritingItemAtURL:url
                                        options:NSFileCoordinatorWritingForMoving
                                          error:&stageError
                                     byAccessor:^(NSURL* coordinatedUrl) {
                                         moved = [manager moveItemAtURL:coordinatedUrl
                                                                  toURL:destinationUrl
                                                                  error:&moveError];
                                     }];
        if (!moved) {
            error = error_message(stageError ?: moveError, "Unable to retain the imported file");
            return {};
        }
        batch->files.push_back(destination);
    }
    return batch;
}

io::JoinResult retain_ios_import(
    const std::filesystem::path& source, const std::filesystem::path& root) {
    std::string hash = file_digest(source);
    if (hash.empty()) {
        return {.status = io::Status::Failed, .message = "Unable to hash the imported file"};
    }
    NSFileManager* manager = NSFileManager.defaultManager;
    NSString* sourcePath = ns_path(source);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto directory = root / hash;
        const auto destination = directory / source.filename();
        NSString* destinationPath = ns_path(destination);
        const std::string location =
            std::string{borealis::io::detail::AppleImportPrefix} + hash + "/" + io::fs_path_to_string(source.filename());
        NSError* error = nil;
        if (![manager createDirectoryAtPath:ns_path(directory)
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:&error])
        {
            return {.status = io::Status::Failed,
                .message = error_message(error, "Unable to create import storage")};
        }
        if ([manager moveItemAtPath:sourcePath toPath:destinationPath error:&error]) {
            return {.status = io::Status::Ok, .location = location};
        }
        if (![manager fileExistsAtPath:destinationPath]) {
            return {.status = io::Status::Failed,
                .message = error_message(error, "Unable to retain the imported file")};
        }
        if ([manager contentsEqualAtPath:sourcePath andPath:destinationPath]) {
            return {.status = io::Status::Ok, .location = location};
        }
        hash = unique_digest();
    }
    return {.status = io::Status::Failed, .message = "Unable to reserve an imported file location"};
}

}  // namespace borealis::file_select::detail
