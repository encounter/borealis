#include "file_select_internal.hpp"

#include "../io_internal.hpp"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <utility>

using borealis::file_select::Callback;
using borealis::file_select::Result;
using borealis::file_select::Status;
using borealis::file_select::detail::complete;

namespace {

void* gPickerDelegateKey = &gPickerDelegateKey;

struct IOSFileState {
    Callback callback;
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

    auto state = std::make_unique<IOSFileState>(IOSFileState{.callback = std::move(callback)});
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

}  // namespace borealis::file_select::detail
