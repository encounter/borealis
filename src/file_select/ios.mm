#include "file_select_internal.hpp"

#include "../io_internal.hpp"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <filesystem>
#include <memory>
#include <utility>

using borealis::file_select::Callback;
using borealis::file_select::Result;
using borealis::file_select::Status;
using borealis::file_select::detail::complete;

namespace {

void* gPickerDelegateKey = &gPickerDelegateKey;

std::string error_message(NSError* error, const char* fallback) {
    const char* description = error.localizedDescription.UTF8String;
    return description != nullptr ? description : fallback;
}

struct IOSFileState {
    Callback callback;
    borealis::io::PathAccess sourceAccess;
    std::filesystem::path temporaryDirectory;

    ~IOSFileState() {
        if (!temporaryDirectory.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(temporaryDirectory, ignored);
        }
    }
};

UIViewController* top_view_controller(UIViewController* controller) {
    UIViewController* current = controller;
    while (current.presentedViewController != nil) {
        current = current.presentedViewController;
    }
    return current;
}

UIViewController* presenter_from_window(SDL_Window* window) {
    if (window == nullptr) {
        return nil;
    }

    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    if (properties == 0) {
        return nil;
    }

    UIWindow* uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (uiWindow == nil || uiWindow.rootViewController == nil) {
        return nil;
    }
    return top_view_controller(uiWindow.rootViewController);
}

NSURL* initial_directory_url(const std::string& defaultLocation) {
    if (defaultLocation.empty()) {
        return nil;
    }

    std::string resolvedPath = defaultLocation;
    void* access = nullptr;
    if (defaultLocation.starts_with("bookmark://")) {
        auto resolved = borealis::io::detail::resolve_apple_bookmark(defaultLocation, true);
        if (resolved.status != borealis::io::Status::Ok) {
            return nil;
        }
        resolvedPath = std::move(resolved.path);
        access = resolved.access;
    }
    NSString* path = [NSString stringWithUTF8String:resolvedPath.c_str()];
    if (path == nil) {
        borealis::io::detail::release_access(access);
        return nil;
    }
    NSURL* url = [NSURL fileURLWithPath:path];
    NSURL* directory = [path hasSuffix:@"/"] ? url : url.URLByDeletingLastPathComponent;
    borealis::io::detail::release_access(access);
    return directory;
}

}  // namespace

@interface BorealisDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>

@property(nonatomic, assign) IOSFileState* state;

@end

@implementation BorealisDocumentPickerDelegate

- (void)finishWithResult:(Result)result {
    if (self.state == nullptr) {
        return;
    }
    std::unique_ptr<IOSFileState> state{self.state};
    self.state = nullptr;
    complete(std::move(state->callback), std::move(result));
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    Result result{.status = Status::Selected};
    for (NSURL* url in urls) {
        const BOOL accessing = [url startAccessingSecurityScopedResource];
        std::string error;
        std::string bookmark = borealis::io::detail::apple_bookmark_for_url(
            (__bridge void*)url, error);
        if (accessing) {
            [url stopAccessingSecurityScopedResource];
        }
        if (bookmark.empty()) {
            result.status = Status::Failed;
            result.locations.clear();
            result.message = std::move(error);
            break;
        }
        result.locations.push_back(std::move(bookmark));
    }

    if (result.locations.empty()) {
        result.status = Status::Failed;
        result.message = "The selected files could not be represented as UTF-8";
    }
    [self finishWithResult:std::move(result)];
    (void)controller;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    [self finishWithResult:Result{.status = Status::Canceled}];
    (void)controller;
}

@end

namespace borealis::file_select::detail {

namespace {

void open_ios_picker(SDL_Window* parentWindow, std::string defaultLocation, bool multiSelect,
    NSArray<UTType*>* contentTypes, Callback callback) {
    UIViewController* presenter = presenter_from_window(parentWindow);
    if (presenter == nil) {
        complete(std::move(callback), {
            .status = Status::Failed,
            .message = "Unable to find an iOS view controller for the file dialog",
        });
        return;
    }

    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:contentTypes
                           asCopy:NO];
    picker.allowsMultipleSelection = multiSelect ? YES : NO;
    picker.shouldShowFileExtensions = YES;
    if (NSURL* directoryUrl = initial_directory_url(defaultLocation)) {
        picker.directoryURL = directoryUrl;
    }

    auto state = std::make_unique<IOSFileState>();
    state->callback = std::move(callback);
    BorealisDocumentPickerDelegate* delegate = [BorealisDocumentPickerDelegate new];
    delegate.state = state.release();
    picker.delegate = delegate;
    objc_setAssociatedObject(
        picker, gPickerDelegateKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [presenter presentViewController:picker animated:YES completion:nil];
}

}  // namespace

void open_ios_file(FileOptions options, Callback callback) {
    open_ios_picker(options.parentWindow, std::move(options.defaultLocation), options.multiSelect,
        @[ UTTypeItem ], std::move(callback));
}

void open_ios_folder(FolderOptions options, Callback callback) {
    open_ios_picker(options.parentWindow, std::move(options.defaultLocation), false,
        @[ UTTypeFolder ], std::move(callback));
}

void export_ios_file(ExportOptions options, Callback callback) {
    UIViewController* presenter = presenter_from_window(options.parentWindow);
    if (presenter == nil) {
        complete(std::move(callback), {
            .status = Status::Failed,
            .message = "Unable to find an iOS view controller for the export dialog",
        });
        return;
    }

    auto state = std::make_unique<IOSFileState>();
    state->callback = std::move(callback);
    state->sourceAccess = borealis::io::access_path(options.sourceLocation);
    if (!state->sourceAccess) {
        complete(std::move(state->callback), {
            .status = Status::Failed,
            .message = "Export source is not available as an iOS file URL",
        });
        return;
    }

    std::filesystem::path exportPath = state->sourceAccess.path();
    if (borealis::io::fs_path_to_string(exportPath.filename()) != options.suggestedName) {
        NSString* temporaryRoot = NSTemporaryDirectory();
        NSString* identifier = NSUUID.UUID.UUIDString;
        NSString* directory = [temporaryRoot stringByAppendingPathComponent:identifier];
        NSString* suggestedName = [[NSString alloc] initWithBytes:options.suggestedName.data()
                                                            length:options.suggestedName.size()
                                                          encoding:NSUTF8StringEncoding];
        if (suggestedName == nil) {
            complete(std::move(state->callback), {
                .status = Status::Failed,
                .message = "Suggested export name is not valid UTF-8",
            });
            return;
        }
        NSError* stageError = nil;
        NSFileManager* manager = NSFileManager.defaultManager;
        if (![manager createDirectoryAtPath:directory
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:&stageError])
        {
            complete(std::move(state->callback), {
                .status = Status::Failed,
                .message = error_message(stageError, "Unable to stage exported file"),
            });
            return;
        }
        state->temporaryDirectory = borealis::io::fs_path_from_utf8(directory.UTF8String);
        NSString* source = [NSString stringWithUTF8String:
            borealis::io::fs_path_to_string(exportPath).c_str()];
        NSString* destination = [directory stringByAppendingPathComponent:suggestedName];
        if (source == nil || ![manager copyItemAtPath:source toPath:destination error:&stageError]) {
            complete(std::move(state->callback), {
                .status = Status::Failed,
                .message = error_message(stageError, "Unable to stage exported file"),
            });
            return;
        }
        exportPath = borealis::io::fs_path_from_utf8(destination.UTF8String);
    }

    NSString* path = [NSString stringWithUTF8String:
        borealis::io::fs_path_to_string(exportPath).c_str()];
    if (path == nil) {
        complete(std::move(state->callback), {
            .status = Status::Failed,
            .message = "Export source path is not valid UTF-8",
        });
        return;
    }
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc]
        initForExportingURLs:@[ [NSURL fileURLWithPath:path] ]
                      asCopy:YES];
    picker.shouldShowFileExtensions = YES;
    BorealisDocumentPickerDelegate* delegate = [BorealisDocumentPickerDelegate new];
    delegate.state = state.release();
    picker.delegate = delegate;
    objc_setAssociatedObject(
        picker, gPickerDelegateKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [presenter presentViewController:picker animated:YES completion:nil];
}

}  // namespace borealis::file_select::detail
