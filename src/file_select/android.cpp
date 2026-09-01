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

bool show_folder_dialog(AndroidFolderState* state, bool requireRealPath) {
    jni::LocalFrame frame{jni::env()};
    if (!frame) {
        return false;
    }
    auto method = jni::activity_method("showFolderDialog", "(JZ)Z");
    if (!method) {
        return false;
    }
    const jboolean shown = method.env->CallBooleanMethod(
        method.activity, method.method, reinterpret_cast<jlong>(state),
        requireRealPath ? JNI_TRUE : JNI_FALSE);
    if (jni::clear_pending_exception(method.env)) {
        return false;
    }
    return shown == JNI_TRUE;
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
