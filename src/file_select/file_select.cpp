#include "borealis/file_select.hpp"

#include "borealis/io.hpp"

#include "../io_internal.hpp"
#include "file_select_internal.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>

#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
#define BOREALIS_USE_IOS_FILE_DIALOG 1
#else
#define BOREALIS_USE_IOS_FILE_DIALOG 0
#endif

#if defined(__APPLE__) && !TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST
#define BOREALIS_USE_MACOS_FOLDER_DIALOG 1
#else
#define BOREALIS_USE_MACOS_FOLDER_DIALOG 0
#endif

namespace borealis::file_select {
namespace detail {
namespace {

struct CompletionState {
    Callback callback;
    Result result;
};

std::atomic_bool g_dialogBusy = false;

void invoke_completion(void* userdata) {
    std::unique_ptr<CompletionState> state{static_cast<CompletionState*>(userdata)};
    state->callback(std::move(state->result));
}

}  // namespace

void dispatch_completion(Callback callback, Result result) {
    if (!callback) {
        return;
    }

    auto state = std::make_unique<CompletionState>(
        CompletionState{.callback = std::move(callback), .result = std::move(result)});
    if (SDL_RunOnMainThread(&invoke_completion, state.get(), false)) {
        state.release();
        return;
    }

    // Run inline if SDL cannot queue the callback.
    invoke_completion(state.release());
}

void complete(Callback callback, Result result) {
    g_dialogBusy.store(false, std::memory_order_release);
    dispatch_completion(std::move(callback), std::move(result));
}

void reject(Callback callback, Result result) {
    dispatch_completion(std::move(callback), std::move(result));
}

bool acquire_dialog() {
    bool expected = false;
    return g_dialogBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
}

Result result_from_file_list(const char* const* fileList, const char* error) {
    if (fileList == nullptr) {
        return {
            .status = Status::Failed,
            .message = error == nullptr || error[0] == '\0' ? "File dialog failed" : error,
        };
    }
    if (fileList[0] == nullptr) {
        return {.status = Status::Canceled};
    }

    Result result{.status = Status::Selected};
    for (const char* const* location = fileList; *location != nullptr; ++location) {
        result.locations.emplace_back(*location);
    }
    return result;
}

Result copy_export_file(
    std::string_view sourceLocation, std::string_view destinationLocation, bool atomic) {
    if (sourceLocation == destinationLocation) {
        return {.status = Status::Selected, .locations = {std::string{destinationLocation}}};
    }

    auto copy = [](std::string_view source, std::string_view destination) -> Result {
        auto input = io::open(source, io::File::Mode::Read);
        if (input.status != io::Status::Ok) {
            return {.status = Status::Failed,
                .message = input.message.empty() ? "Unable to open export source" : input.message};
        }
        auto output = io::open(destination, io::File::Mode::Truncate);
        if (output.status != io::Status::Ok) {
            return {.status = Status::Failed,
                .message =
                    output.message.empty() ? "Unable to open export destination" : output.message};
        }

        std::array<std::byte, 64 * 1024> buffer{};
        while (true) {
            const uint64_t read = input.file.read(buffer.data(), buffer.size());
            if (read == 0) {
                if (!input.file.error().empty()) {
                    return {.status = Status::Failed, .message = input.file.error()};
                }
                break;
            }
            if (!output.file.write(std::span{buffer.data(), static_cast<size_t>(read)})) {
                return {.status = Status::Failed, .message = output.file.error()};
            }
        }
        if (!output.file.close()) {
            return {.status = Status::Failed, .message = output.file.error()};
        }
        return {.status = Status::Selected, .locations = {std::string{destination}}};
    };

    try {
        if (!atomic) {
            return copy(sourceLocation, destinationLocation);
        }

        const std::filesystem::path destination = io::fs_path_from_utf8(destinationLocation);
        std::filesystem::path parent = destination.parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        static std::atomic_uint64_t sequence = 0;
        io::JoinResult temporary;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::string name = "." + io::fs_path_to_string(destination.filename()) +
                                     ".borealis-export-" + std::to_string(sequence.fetch_add(1)) +
                                     ".tmp";
            temporary = io::create_child(io::fs_path_to_string(parent), name);
            if (temporary.status != io::Status::AlreadyExists) {
                break;
            }
        }
        if (temporary.status != io::Status::Ok) {
            return {.status = Status::Failed,
                .message = temporary.message.empty() ? "Unable to stage exported file" :
                                                       temporary.message};
        }

        Result result = copy(sourceLocation, temporary.location);
        if (result.status != Status::Selected) {
            std::error_code ignored;
            std::filesystem::remove(io::fs_path_from_utf8(temporary.location), ignored);
            return result;
        }
        std::string error;
        if (!io::atomic_replace(io::fs_path_from_utf8(temporary.location), destination, error)) {
            std::error_code ignored;
            std::filesystem::remove(io::fs_path_from_utf8(temporary.location), ignored);
            return {.status = Status::Failed, .message = std::move(error)};
        }
        return {.status = Status::Selected, .locations = {std::string{destinationLocation}}};
    } catch (const std::exception& exception) {
        return {.status = Status::Failed, .message = exception.what()};
    } catch (...) {
        return {.status = Status::Failed, .message = "Unable to export file"};
    }
}

namespace {

struct SDLDialogState {
    Callback callback;
    std::vector<Filter> filters;
    std::vector<SDL_DialogFileFilter> sdlFilters;
    std::string defaultLocation;
};

void sdl_dialog_finished(void* userdata, const char* const* fileList, int) {
    std::unique_ptr<SDLDialogState> state{static_cast<SDLDialogState*>(userdata)};
    const char* error = fileList == nullptr ? SDL_GetError() : nullptr;
    complete(std::move(state->callback), result_from_file_list(fileList, error));
}

struct SDLExportState {
    Callback callback;
    std::string sourceLocation;
    std::vector<Filter> filters;
    std::vector<SDL_DialogFileFilter> sdlFilters;
    std::string suggestedName;
};

void sdl_export_finished(void* userdata, const char* const* fileList, int) {
    std::unique_ptr<SDLExportState> state{static_cast<SDLExportState*>(userdata)};
    Result selected =
        result_from_file_list(fileList, fileList == nullptr ? SDL_GetError() : nullptr);
    if (selected.status != Status::Selected) {
        complete(std::move(state->callback), std::move(selected));
        return;
    }
    complete(std::move(state->callback),
        copy_export_file(state->sourceLocation, selected.locations.front(), true));
}

std::unique_ptr<SDLExportState> make_sdl_export_state(ExportOptions options, Callback callback) {
    auto state = std::make_unique<SDLExportState>();
    state->callback = std::move(callback);
    state->sourceLocation = std::move(options.sourceLocation);
    state->filters = std::move(options.filters);
    state->suggestedName = std::move(options.suggestedName);
    state->sdlFilters.reserve(state->filters.size());
    for (const auto& filter : state->filters) {
        state->sdlFilters.push_back({filter.name.c_str(), filter.pattern.c_str()});
    }
    return state;
}

std::unique_ptr<SDLDialogState> make_sdl_state(
    Callback callback, std::vector<Filter> filters, std::string defaultLocation) {
    auto state = std::make_unique<SDLDialogState>();
    state->callback = std::move(callback);
    state->filters = std::move(filters);
    state->defaultLocation = std::move(defaultLocation);
    state->sdlFilters.reserve(state->filters.size());
    for (const auto& filter : state->filters) {
        state->sdlFilters.push_back({filter.name.c_str(), filter.pattern.c_str()});
    }
    return state;
}

void fail_wrong_thread(Callback callback) {
    reject(
        std::move(callback), {
                                 .status = Status::Failed,
                                 .message = "File selection must be started on SDL's main thread",
                             });
}

}  // namespace
}  // namespace detail

Capabilities capabilities() noexcept {
#if defined(__APPLE__) && TARGET_OS_TV
    return {};
#else
    return {.canOpenFile = true, .canOpenFolder = true, .canExportFile = true};
#endif
}

bool busy() noexcept {
    return detail::g_dialogBusy.load(std::memory_order_acquire);
}

void open_file(FileOptions options, Callback callback) {
    if (!callback) {
        return;
    }
    if (!SDL_IsMainThread()) {
        detail::fail_wrong_thread(std::move(callback));
        return;
    }
    if (!capabilities().canOpenFile) {
        detail::reject(
            std::move(callback), {
                                     .status = Status::Unsupported,
                                     .message = "File selection is not supported on this platform",
                                 });
        return;
    }
    if (!detail::acquire_dialog()) {
        detail::reject(std::move(callback), {
                                                .status = Status::Busy,
                                                .message = "Another file dialog is already open",
                                            });
        return;
    }

#if BOREALIS_USE_IOS_FILE_DIALOG
    detail::open_ios_file(std::move(options), std::move(callback));
#else
    const int filterCount = static_cast<int>(options.filters.size());
    auto state = detail::make_sdl_state(
        std::move(callback), std::move(options.filters), std::move(options.defaultLocation));
    const auto* filters = state->sdlFilters.empty() ? nullptr : state->sdlFilters.data();
    const auto* defaultLocation =
        state->defaultLocation.empty() ? nullptr : state->defaultLocation.c_str();
    SDL_ShowOpenFileDialog(&detail::sdl_dialog_finished, state.release(), options.parentWindow,
        filters, filterCount, defaultLocation, options.multiSelect);
#endif
}

void open_folder(FolderOptions options, Callback callback) {
    if (!callback) {
        return;
    }
    if (!SDL_IsMainThread()) {
        detail::fail_wrong_thread(std::move(callback));
        return;
    }
    if (!capabilities().canOpenFolder) {
        detail::reject(std::move(callback),
            {
                .status = Status::Unsupported,
                .message = "Folder selection is not supported on this platform",
            });
        return;
    }
    if (!detail::acquire_dialog()) {
        detail::reject(std::move(callback), {
                                                .status = Status::Busy,
                                                .message = "Another file dialog is already open",
                                            });
        return;
    }

#if BOREALIS_USE_IOS_FILE_DIALOG
    detail::open_ios_folder(std::move(options), std::move(callback));
#elif BOREALIS_USE_MACOS_FOLDER_DIALOG
    detail::open_macos_folder(std::move(options), std::move(callback));
#elif defined(__ANDROID__) || defined(ANDROID)
    detail::open_android_folder(std::move(options), std::move(callback));
#else
    auto state =
        detail::make_sdl_state(std::move(callback), {}, std::move(options.defaultLocation));
    const auto* defaultLocation =
        state->defaultLocation.empty() ? nullptr : state->defaultLocation.c_str();
    SDL_ShowOpenFolderDialog(&detail::sdl_dialog_finished, state.release(), options.parentWindow,
        defaultLocation, false);
#endif
}

void export_file(ExportOptions options, Callback callback) {
    if (!callback) {
        return;
    }
    if (!SDL_IsMainThread()) {
        detail::fail_wrong_thread(std::move(callback));
        return;
    }
    if (!capabilities().canExportFile) {
        detail::reject(
            std::move(callback), {
                                     .status = Status::Unsupported,
                                     .message = "File export is not supported on this platform",
                                 });
        return;
    }
    if (!io::detail::safe_child_name(options.suggestedName)) {
        detail::reject(
            std::move(callback), {
                                     .status = Status::Failed,
                                     .message = "Suggested name must be a safe file name",
                                 });
        return;
    }
    if (io::check(options.sourceLocation) != io::Status::Ok) {
        detail::reject(std::move(callback), {
                                                .status = Status::Failed,
                                                .message = "Export source is unavailable",
                                            });
        return;
    }
    if (!detail::acquire_dialog()) {
        detail::reject(std::move(callback), {
                                                .status = Status::Busy,
                                                .message = "Another file dialog is already open",
                                            });
        return;
    }

#if BOREALIS_USE_IOS_FILE_DIALOG
    detail::export_ios_file(std::move(options), std::move(callback));
#elif defined(__ANDROID__) || defined(ANDROID)
    detail::export_android_file(std::move(options), std::move(callback));
#else
    SDL_Window* parentWindow = options.parentWindow;
    const int filterCount = static_cast<int>(options.filters.size());
    auto state = detail::make_sdl_export_state(std::move(options), std::move(callback));
    const auto* filters = state->sdlFilters.empty() ? nullptr : state->sdlFilters.data();
    const char* suggestedName = state->suggestedName.c_str();
    SDL_ShowSaveFileDialog(&detail::sdl_export_finished, state.release(), parentWindow, filters,
        filterCount, suggestedName);
#endif
}

}  // namespace borealis::file_select
