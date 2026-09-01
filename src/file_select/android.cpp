#include "file_select_internal.hpp"

#include "../jni_internal.hpp"

#include <jni.h>

#include <memory>
#include <string>
#include <utility>

namespace borealis::file_select::detail {
namespace {

struct AndroidFolderState {
    Callback callback;
};

struct AndroidExportState {
    Callback callback;
    std::string sourceLocation;
};

bool show_folder_dialog(AndroidFolderState* state, bool requireRealPath) {
    jni::LocalFrame frame{jni::env()};
    if (!frame) {
        return false;
    }
    auto method = jni::activity_method("showFolderDialog", "(JZ)Z");
    if (!method) {
        return false;
    }
    const jboolean shown = method.env->CallBooleanMethod(method.activity, method.method,
        reinterpret_cast<jlong>(state), requireRealPath ? JNI_TRUE : JNI_FALSE);
    if (jni::clear_pending_exception(method.env)) {
        return false;
    }
    return shown == JNI_TRUE;
}

bool show_export_dialog(AndroidExportState* state, const ExportOptions& options) {
    auto* env = jni::env();
    jni::LocalFrame frame{env, static_cast<jint>(options.filters.size() + 16)};
    if (!frame) {
        return false;
    }
    auto method =
        jni::activity_method("showExportDialog", "(JLjava/lang/String;[Ljava/lang/String;)Z");
    if (!method) {
        return false;
    }
    jstring suggestedName = jni::make_string(env, options.suggestedName);
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray filters =
        stringClass == nullptr ?
            nullptr :
            env->NewObjectArray(static_cast<jsize>(options.filters.size()), stringClass, nullptr);
    if (suggestedName == nullptr || filters == nullptr || jni::clear_pending_exception(env)) {
        return false;
    }
    for (size_t i = 0; i < options.filters.size(); ++i) {
        jstring pattern = jni::make_string(env, options.filters[i].pattern);
        if (pattern == nullptr) {
            return false;
        }
        env->SetObjectArrayElement(filters, static_cast<jsize>(i), pattern);
        if (jni::clear_pending_exception(env)) {
            return false;
        }
    }
    const jboolean shown = env->CallBooleanMethod(
        method.activity, method.method, reinterpret_cast<jlong>(state), suggestedName, filters);
    if (jni::clear_pending_exception(env)) {
        return false;
    }
    return shown == JNI_TRUE;
}

void delete_export_destination(std::string_view location) {
    jni::LocalFrame frame{jni::env()};
    if (!frame) {
        return;
    }
    auto method = jni::activity_method("deleteDocumentUri", "(Ljava/lang/String;)Z");
    if (!method) {
        return;
    }
    jstring uri = jni::make_string(method.env, location);
    if (uri != nullptr) {
        method.env->CallBooleanMethod(method.activity, method.method, uri);
        jni::clear_pending_exception(method.env);
    }
}

}  // namespace

void open_android_folder(FolderOptions options, Callback callback) {
    auto state =
        std::make_unique<AndroidFolderState>(AndroidFolderState{.callback = std::move(callback)});
    if (show_folder_dialog(state.get(), options.requireRealPath)) {
        state.release();
        return;
    }

    complete(std::move(state->callback), {
                                             .status = Status::Failed,
                                             .message = "Unable to open the Android folder dialog",
                                         });
}

void export_android_file(ExportOptions options, Callback callback) {
    auto state = std::make_unique<AndroidExportState>(AndroidExportState{
        .callback = std::move(callback),
        .sourceLocation = std::move(options.sourceLocation),
    });
    if (show_export_dialog(state.get(), options)) {
        state.release();
        return;
    }

    complete(std::move(state->callback), {
                                             .status = Status::Failed,
                                             .message = "Unable to open the Android export dialog",
                                         });
}

}  // namespace borealis::file_select::detail

extern "C" JNIEXPORT void JNICALL
Java_dev_encounter_borealis_BorealisActivity_nativeFolderDialogResult(
    JNIEnv* env, jclass, jlong userdata, jstring path, jstring error) {
    using namespace borealis::file_select;
    using namespace borealis::file_select::detail;

    std::unique_ptr<AndroidFolderState> state{reinterpret_cast<AndroidFolderState*>(userdata)};
    if (state == nullptr) {
        return;
    }

    const std::string pathString = borealis::jni::to_string(env, path);
    const std::string errorString = borealis::jni::to_string(env, error);
    if (!errorString.empty()) {
        complete(std::move(state->callback), {
                                                 .status = Status::Failed,
                                                 .message = errorString,
                                             });
    } else if (pathString.empty()) {
        complete(std::move(state->callback), {.status = Status::Canceled});
    } else {
        complete(std::move(state->callback), {
                                                 .status = Status::Selected,
                                                 .locations = {pathString},
                                             });
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_encounter_borealis_BorealisActivity_nativeExportDialogResult(
    JNIEnv* env, jclass, jlong userdata, jstring path, jstring error) {
    using namespace borealis::file_select;
    using namespace borealis::file_select::detail;

    std::unique_ptr<AndroidExportState> state{reinterpret_cast<AndroidExportState*>(userdata)};
    if (state == nullptr) {
        return;
    }

    const std::string pathString = borealis::jni::to_string(env, path);
    const std::string errorString = borealis::jni::to_string(env, error);
    if (!errorString.empty()) {
        complete(std::move(state->callback), {
                                                 .status = Status::Failed,
                                                 .message = errorString,
                                             });
    } else if (pathString.empty()) {
        complete(std::move(state->callback), {.status = Status::Canceled});
    } else {
        Result result = copy_export_file(state->sourceLocation, pathString, false);
        if (result.status != Status::Selected) {
            delete_export_destination(pathString);
        }
        complete(std::move(state->callback), std::move(result));
    }
}
