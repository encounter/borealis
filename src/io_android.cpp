#include "io_internal.hpp"

#include "jni_internal.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <jni.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace borealis::io::detail {
namespace {

struct AndroidWriteStream {
    int fd = -1;
};

void set_fd_error(const char* action) {
    SDL_SetError("%s: %s", action, std::generic_category().message(errno).c_str());
}

Sint64 SDLCALL android_write_size(void* userdata) {
    struct stat info{};
    if (fstat(static_cast<AndroidWriteStream*>(userdata)->fd, &info) != 0) {
        set_fd_error("Unable to determine Android document size");
        return -1;
    }
    return static_cast<Sint64>(info.st_size);
}

Sint64 SDLCALL android_write_seek(void* userdata, Sint64 offset, SDL_IOWhence whence) {
    int origin = SEEK_SET;
    if (whence == SDL_IO_SEEK_CUR) {
        origin = SEEK_CUR;
    } else if (whence == SDL_IO_SEEK_END) {
        origin = SEEK_END;
    }
    const off_t result =
        lseek(static_cast<AndroidWriteStream*>(userdata)->fd, static_cast<off_t>(offset), origin);
    if (result < 0) {
        set_fd_error("Unable to seek Android document");
        return -1;
    }
    return static_cast<Sint64>(result);
}

size_t SDLCALL android_write(void* userdata, const void* data, size_t size, SDL_IOStatus* status) {
    auto* stream = static_cast<AndroidWriteStream*>(userdata);
    const auto* bytes = static_cast<const std::byte*>(data);
    size_t written = 0;
    while (written < size) {
        const size_t remaining =
            std::min(size - written, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t result = ::write(stream->fd, bytes + written, remaining);
        if (result > 0) {
            written += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = EIO;
        }
        set_fd_error("Unable to write Android document");
        *status = SDL_IO_STATUS_ERROR;
        break;
    }
    return written;
}

bool SDLCALL android_write_flush(void* userdata, SDL_IOStatus* status) {
    while (fsync(static_cast<AndroidWriteStream*>(userdata)->fd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        set_fd_error("Unable to flush Android document");
        *status = SDL_IO_STATUS_ERROR;
        return false;
    }
    return true;
}

bool SDLCALL android_write_close(void* userdata) {
    std::unique_ptr<AndroidWriteStream> stream{static_cast<AndroidWriteStream*>(userdata)};
    if (close(stream->fd) != 0) {
        set_fd_error("Unable to close Android document");
        return false;
    }
    return true;
}

}  // namespace

NativeOpenResult android_open_write(std::string_view location, File::Mode mode) {
    auto* env = jni::env();
    jni::LocalFrame frame{env};
    if (!frame) {
        return {.message = "Android document access is unavailable"};
    }
    auto method =
        jni::activity_method("openUriFileDescriptor", "(Ljava/lang/String;Ljava/lang/String;)I");
    if (!method) {
        return {.message = "Android document access is unavailable"};
    }
    jstring uri = jni::make_string(env, location);
    jstring openMode = jni::make_string(env, mode == File::Mode::Append ? "wa" : "wt");
    if (uri == nullptr || openMode == nullptr) {
        return {.message = "Unable to encode Android document location"};
    }
    const jint fd = env->CallIntMethod(method.activity, method.method, uri, openMode);
    if (jni::clear_pending_exception(env) || fd < 0) {
        return {.status = Status::NotFound, .message = "Unable to open Android document"};
    }

    auto stream = std::make_unique<AndroidWriteStream>();
    stream->fd = fd;
    SDL_IOStreamInterface interface{};
    SDL_INIT_INTERFACE(&interface);
    interface.size = android_write_size;
    interface.seek = android_write_seek;
    interface.write = android_write;
    interface.flush = android_write_flush;
    interface.close = android_write_close;
    SDL_IOStream* handle = SDL_OpenIO(&interface, stream.get());
    if (handle == nullptr) {
        close(fd);
        return {.message = SDL_GetError()};
    }
    stream.release();
    return {.status = Status::Ok, .handle = handle};
}

Status android_check(std::string_view location) {
    auto* env = jni::env();
    jni::LocalFrame frame{env};
    if (!frame) {
        return Status::Failed;
    }
    auto method = jni::activity_method("checkUri", "(Ljava/lang/String;)Z");
    if (!method) {
        return Status::Failed;
    }
    jstring uri = jni::make_string(env, location);
    if (uri == nullptr) {
        return Status::Failed;
    }
    const jboolean available = env->CallBooleanMethod(method.activity, method.method, uri);
    if (jni::clear_pending_exception(env)) {
        return Status::Failed;
    }
    return available == JNI_TRUE ? Status::Ok : Status::NotFound;
}

std::string android_display_name(std::string_view location) {
    auto* env = jni::env();
    jni::LocalFrame frame{env};
    if (!frame) {
        return {};
    }
    auto method =
        jni::activity_method("getDisplayNameForUri", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!method) {
        return {};
    }
    jstring uri = jni::make_string(env, location);
    if (uri == nullptr) {
        return {};
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(method.activity, method.method, uri));
    if (name == nullptr || jni::clear_pending_exception(env)) {
        return {};
    }
    return jni::to_string(env, name);
}

JoinResult android_join(std::string_view folder, std::string_view relativePath) {
    auto* env = jni::env();
    jni::LocalFrame frame{env};
    if (!frame) {
        return {.status = Status::Failed, .message = "Android document traversal is unavailable"};
    }
    auto method = jni::activity_method(
        "joinDocumentUri", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!method) {
        return {.status = Status::Failed, .message = "Android document traversal is unavailable"};
    }
    jstring folderString = jni::make_string(env, folder);
    jstring relativeString = jni::make_string(env, relativePath);
    if (folderString == nullptr || relativeString == nullptr) {
        return {.status = Status::Failed, .message = "Unable to encode Android document path"};
    }
    auto child = static_cast<jstring>(
        env->CallObjectMethod(method.activity, method.method, folderString, relativeString));
    if (jni::clear_pending_exception(env)) {
        return {.status = Status::Failed, .message = "Android document traversal failed"};
    }
    if (child == nullptr) {
        return {.status = Status::NotFound, .message = "Child does not exist"};
    }
    std::string result = jni::to_string(env, child);
    return result.empty() ?
               JoinResult{.status = Status::NotFound, .message = "Child does not exist"} :
               JoinResult{.status = Status::Ok, .location = std::move(result)};
}

JoinResult android_create_child(std::string_view folder, std::string_view name) {
    auto* env = jni::env();
    jni::LocalFrame frame{env};
    if (!frame) {
        return {.status = Status::Failed, .message = "Android document creation is unavailable"};
    }
    auto method = jni::activity_method(
        "createDocumentUri", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!method) {
        return {.status = Status::Failed, .message = "Android document creation is unavailable"};
    }
    jstring folderString = jni::make_string(env, folder);
    jstring nameString = jni::make_string(env, name);
    if (folderString == nullptr || nameString == nullptr) {
        return {.status = Status::Failed, .message = "Unable to encode Android document name"};
    }
    auto child = static_cast<jstring>(
        env->CallObjectMethod(method.activity, method.method, folderString, nameString));
    if (jni::clear_pending_exception(env) || child == nullptr) {
        return {.status = Status::Failed, .message = "Unable to create Android document"};
    }
    std::string result = jni::to_string(env, child);
    return result.empty() ? JoinResult{.status = Status::Failed,
                                .message = "Unable to create Android document"} :
                            JoinResult{.status = Status::Ok, .location = std::move(result)};
}

ListResult android_list(std::string_view folder) {
    auto* env = jni::env();
    jni::LocalFrame frame{env};
    if (!frame) {
        return {.status = Status::Failed, .message = "Android document traversal is unavailable"};
    }
    auto method =
        jni::activity_method("listDocumentUri", "(Ljava/lang/String;)[Ljava/lang/String;");
    if (!method) {
        return {.status = Status::Failed, .message = "Android document traversal is unavailable"};
    }
    jstring folderString = jni::make_string(env, folder);
    if (folderString == nullptr) {
        return {.status = Status::Failed, .message = "Unable to encode Android folder URI"};
    }
    auto values = static_cast<jobjectArray>(
        env->CallObjectMethod(method.activity, method.method, folderString));
    if (jni::clear_pending_exception(env)) {
        return {.status = Status::Failed, .message = "Android folder enumeration failed"};
    }
    if (values == nullptr) {
        return {.status = Status::NotFound, .message = "Folder is unavailable"};
    }

    const jsize count = env->GetArrayLength(values);
    if (jni::clear_pending_exception(env) || count % 3 != 0) {
        return {.status = Status::Failed, .message = "Android folder result is invalid"};
    }
    ListResult result{.status = Status::Ok};
    result.entries.reserve(static_cast<size_t>(count / 3));
    for (jsize i = 0; i < count; i += 3) {
        auto name = static_cast<jstring>(env->GetObjectArrayElement(values, i));
        auto location = static_cast<jstring>(env->GetObjectArrayElement(values, i + 1));
        auto type = static_cast<jstring>(env->GetObjectArrayElement(values, i + 2));
        if (jni::clear_pending_exception(env)) {
            return {.status = Status::Failed, .message = "Android folder result is invalid"};
        }
        result.entries.push_back({
            .name = jni::to_string(env, name),
            .location = jni::to_string(env, location),
            .isDirectory = jni::to_string(env, type) == "1",
        });
        if (name != nullptr) {
            env->DeleteLocalRef(name);
        }
        if (location != nullptr) {
            env->DeleteLocalRef(location);
        }
        if (type != nullptr) {
            env->DeleteLocalRef(type);
        }
    }
    std::ranges::sort(result.entries, {}, &Entry::name);
    return result;
}

}  // namespace borealis::io::detail
