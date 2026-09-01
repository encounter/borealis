#include "io_internal.hpp"

#include "jni_internal.hpp"

#include <jni.h>

#include <algorithm>
#include <string>
#include <utility>

namespace borealis::io::detail {

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
