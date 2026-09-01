#include "io_internal.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <ranges>
#include <string>

namespace borealis::io::detail {
namespace {

constexpr std::string_view BookmarkPrefix = "bookmark://";

std::string ns_error(NSError* error, const char* fallback) {
    const char* description = error.localizedDescription.UTF8String;
    return description != nullptr ? description : fallback;
}

NSData* decode_bookmark(std::string_view location) {
    if (!location.starts_with(BookmarkPrefix)) {
        return nil;
    }
    std::string encoded{location.substr(BookmarkPrefix.size())};
    std::replace(encoded.begin(), encoded.end(), '-', '+');
    std::replace(encoded.begin(), encoded.end(), '_', '/');
    while (encoded.size() % 4 != 0) {
        encoded.push_back('=');
    }
    NSString* string = [NSString stringWithUTF8String:encoded.c_str()];
    return string == nil ? nil : [[NSData alloc] initWithBase64EncodedString:string options:0];
}

std::string encode_bookmark(NSData* data) {
    NSString* encoded = [data base64EncodedStringWithOptions:0];
    std::string value = encoded.UTF8String != nullptr ? encoded.UTF8String : "";
    std::replace(value.begin(), value.end(), '+', '-');
    std::replace(value.begin(), value.end(), '/', '_');
    while (!value.empty() && value.back() == '=') {
        value.pop_back();
    }
    return std::string{BookmarkPrefix} + value;
}

NSURL* resolve_url(std::string_view location, std::string& error) {
    NSData* bookmark = decode_bookmark(location);
    if (bookmark == nil) {
        error = "Bookmark data is invalid";
        return nil;
    }
    NSURLBookmarkResolutionOptions options = 0;
    BOOL resolvedWithImplicitAccess = YES;
    if (@available(iOS 14.2, *)) {
        options = NSURLBookmarkResolutionWithoutImplicitStartAccessing;
        resolvedWithImplicitAccess = NO;
    }
    NSError* resolveError = nil;
    BOOL stale = NO;
    NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark
                                          options:options
                                    relativeToURL:nil
                              bookmarkDataIsStale:&stale
                                            error:&resolveError];
    if (url == nil) {
        error = ns_error(resolveError, "Unable to resolve bookmark");
        return nil;
    }
    // Before iOS 14.2, bookmark resolution starts ephemeral access automatically. End that
    // access here so callers can explicitly scope any access they need.
    if (resolvedWithImplicitAccess) {
        [url stopAccessingSecurityScopedResource];
    }
    return url;
}

std::string bookmark_for_url(NSURL* url, std::string& error) {
    NSError* bookmarkError = nil;
    NSData* bookmark = [url bookmarkDataWithOptions:0
                     includingResourceValuesForKeys:nil
                                      relativeToURL:nil
                                              error:&bookmarkError];
    if (bookmark == nil) {
        error = ns_error(bookmarkError, "Unable to create bookmark");
        return {};
    }
    return encode_bookmark(bookmark);
}

}  // namespace

ResolvedPath resolve_apple_bookmark(std::string_view location, bool startAccess) {
    std::string error;
    NSURL* url = resolve_url(location, error);
    if (url == nil) {
        return {.status = Status::NotFound, .message = std::move(error)};
    }
    void* access = nullptr;
    if (startAccess) {
        if (![url startAccessingSecurityScopedResource]) {
            return {.status = Status::NotFound, .message = "Bookmark access was denied"};
        }
        access = (__bridge_retained void*)url;
    }
    const char* path = url.path.UTF8String;
    if (path == nullptr) {
        if (access != nullptr) {
            release_access(access);
        }
        return {.status = Status::Failed, .message = "Bookmark path is not valid UTF-8"};
    }
    return {.status = Status::Ok, .path = path, .access = access};
}

void release_access(void* access) noexcept {
    if (access == nullptr) {
        return;
    }
    NSURL* url = (__bridge_transfer NSURL*)access;
    [url stopAccessingSecurityScopedResource];
}

std::string apple_bookmark_for_path(std::string_view path, std::string& error) {
    NSString* value = [[NSString alloc] initWithBytes:path.data()
                                               length:path.size()
                                             encoding:NSUTF8StringEncoding];
    if (value == nil) {
        error = "Path is not valid UTF-8";
        return {};
    }
    return bookmark_for_url([NSURL fileURLWithPath:value], error);
}

std::string apple_bookmark_for_url(void* url, std::string& error) {
    if (url == nullptr) {
        error = "Document URL is invalid";
        return {};
    }
    return bookmark_for_url((__bridge NSURL*)url, error);
}

std::string apple_bookmark_display_name(std::string_view location) {
    std::string error;
    NSURL* url = resolve_url(location, error);
    return url.lastPathComponent.UTF8String != nullptr ? url.lastPathComponent.UTF8String : "";
}

Status apple_bookmark_check(std::string_view location) {
    auto resolved = resolve_apple_bookmark(location, true);
    if (resolved.status != Status::Ok) {
        return resolved.status;
    }
    const bool exists = [[NSFileManager defaultManager]
        fileExistsAtPath:[NSString stringWithUTF8String:resolved.path.c_str()]];
    release_access(resolved.access);
    return exists ? Status::Ok : Status::NotFound;
}

JoinResult apple_bookmark_join(std::string_view folder, std::string_view relativePath) {
    auto resolved = resolve_apple_bookmark(folder, true);
    if (resolved.status != Status::Ok) {
        return {.status = resolved.status, .message = std::move(resolved.message)};
    }
    NSString* base = [NSString stringWithUTF8String:resolved.path.c_str()];
    NSString* relative = [[NSString alloc] initWithBytes:relativePath.data()
                                                  length:relativePath.size()
                                                encoding:NSUTF8StringEncoding];
    if (relative == nil) {
        release_access(resolved.access);
        return {.status = Status::Failed, .message = "Child path is not valid UTF-8"};
    }
    NSString* child = [base stringByAppendingPathComponent:relative];
    if (![[NSFileManager defaultManager] fileExistsAtPath:child]) {
        release_access(resolved.access);
        return {.status = Status::NotFound, .message = "Child does not exist"};
    }
    std::string error;
    const std::string bookmark = bookmark_for_url([NSURL fileURLWithPath:child], error);
    release_access(resolved.access);
    return bookmark.empty() ? JoinResult{.status = Status::Failed, .message = std::move(error)} :
                              JoinResult{.status = Status::Ok, .location = bookmark};
}

JoinResult apple_bookmark_create_child(std::string_view folder, std::string_view name) {
    auto resolved = resolve_apple_bookmark(folder, true);
    if (resolved.status != Status::Ok) {
        return {.status = resolved.status, .message = std::move(resolved.message)};
    }
    NSString* base = [NSString stringWithUTF8String:resolved.path.c_str()];
    NSString* childName = [[NSString alloc] initWithBytes:name.data()
                                                   length:name.size()
                                                 encoding:NSUTF8StringEncoding];
    if (base == nil || childName == nil) {
        release_access(resolved.access);
        return {.status = Status::Failed, .message = "Child name is not valid UTF-8"};
    }
    NSURL* child = [[NSURL fileURLWithPath:base] URLByAppendingPathComponent:childName];
    NSFileManager* manager = [NSFileManager defaultManager];
    if ([manager fileExistsAtPath:child.path]) {
        release_access(resolved.access);
        return {.status = Status::AlreadyExists, .message = "Child already exists"};
    }

    NSError* createError = nil;
    if (![[NSData data] writeToURL:child
                           options:NSDataWritingWithoutOverwriting
                             error:&createError])
    {
        const bool exists = [manager fileExistsAtPath:child.path];
        release_access(resolved.access);
        return exists ? JoinResult{.status = Status::AlreadyExists,
                            .message = "Child already exists"} :
                        JoinResult{.status = Status::Failed,
                            .message = ns_error(createError, "Unable to create child")};
    }

    std::string error;
    const std::string bookmark = bookmark_for_url(child, error);
    if (bookmark.empty()) {
        [manager removeItemAtURL:child error:nil];
    }
    release_access(resolved.access);
    return bookmark.empty() ? JoinResult{.status = Status::Failed, .message = std::move(error)} :
                              JoinResult{.status = Status::Ok, .location = bookmark};
}

ListResult apple_bookmark_list(std::string_view folder) {
    auto resolved = resolve_apple_bookmark(folder, true);
    if (resolved.status != Status::Ok) {
        return {.status = resolved.status, .message = std::move(resolved.message)};
    }
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:resolved.path.c_str()]];
    NSError* listError = nil;
    NSArray<NSURL*>* children = [[NSFileManager defaultManager]
        contentsOfDirectoryAtURL:url
      includingPropertiesForKeys:@[ NSURLIsDirectoryKey ]
                         options:0
                           error:&listError];
    if (children == nil) {
        release_access(resolved.access);
        return {.status = Status::Failed,
            .message = ns_error(listError, "Unable to list folder")};
    }
    ListResult result{.status = Status::Ok};
    for (NSURL* child in children) {
        std::string error;
        const std::string bookmark = bookmark_for_url(child, error);
        if (bookmark.empty()) {
            release_access(resolved.access);
            return {.status = Status::Failed, .message = std::move(error)};
        }
        NSNumber* isDirectory = nil;
        [child getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
        result.entries.push_back({
            .name = child.lastPathComponent.UTF8String != nullptr ?
                        child.lastPathComponent.UTF8String : "",
            .location = bookmark,
            .isDirectory = isDirectory.boolValue,
        });
    }
    release_access(resolved.access);
    std::ranges::sort(result.entries, {}, &Entry::name);
    return result;
}

}  // namespace borealis::io::detail
