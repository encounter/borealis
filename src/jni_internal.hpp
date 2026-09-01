#pragma once

#include <SDL3/SDL_system.h>

#include <jni.h>

#include <string>
#include <string_view>

namespace borealis::jni {

// Returns true if an exception was pending, i.e. the preceding JNI call failed.
inline bool clear_pending_exception(JNIEnv* env) noexcept {
    if (env == nullptr || !env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

inline JNIEnv* env() noexcept {
    return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

// Owns every local ref created between construction and destruction.
class LocalFrame {
public:
    explicit LocalFrame(JNIEnv* env, jint capacity = 32) noexcept : m_env{env} {
        if (m_env != nullptr && m_env->PushLocalFrame(capacity) == 0) {
            m_pushed = true;
        } else {
            clear_pending_exception(m_env);
        }
    }

    ~LocalFrame() {
        if (m_pushed) {
            m_env->PopLocalFrame(nullptr);
        }
    }

    LocalFrame(const LocalFrame&) = delete;
    LocalFrame& operator=(const LocalFrame&) = delete;

    explicit operator bool() const noexcept { return m_pushed; }

private:
    JNIEnv* m_env = nullptr;
    bool m_pushed = false;
};

inline std::string to_string(JNIEnv* env, jstring value) {
    if (env == nullptr || value == nullptr) {
        return {};
    }
    const char* utf8 = env->GetStringUTFChars(value, nullptr);
    if (utf8 == nullptr) {
        clear_pending_exception(env);
        return {};
    }
    std::string result{utf8};
    env->ReleaseStringUTFChars(value, utf8);
    return result;
}

inline jstring make_string(JNIEnv* env, std::string_view value) {
    const std::string copy{value};
    jstring result = env->NewStringUTF(copy.c_str());
    if (clear_pending_exception(env)) {
        return nullptr;
    }
    return result;
}

// Instance method resolved on the SDL activity.
struct ActivityMethod {
    JNIEnv* env = nullptr;
    jobject activity = nullptr;
    jmethodID method = nullptr;

    explicit operator bool() const noexcept { return method != nullptr; }
};

inline ActivityMethod activity_method(const char* name, const char* signature) noexcept {
    ActivityMethod result;
    result.env = env();
    if (result.env == nullptr) {
        return result;
    }
    result.activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (result.activity == nullptr || clear_pending_exception(result.env)) {
        return result;
    }
    jclass activityClass = result.env->GetObjectClass(result.activity);
    if (activityClass == nullptr || clear_pending_exception(result.env)) {
        return result;
    }
    result.method = result.env->GetMethodID(activityClass, name, signature);
    if (clear_pending_exception(result.env)) {
        result.method = nullptr;
    }
    return result;
}

// FindClass on a native thread only sees system classes, so application classes must be loaded
// through the activity's class loader.
inline jclass find_app_class(JNIEnv* env, const char* name) {
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (activity == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    jclass activityClass = env->GetObjectClass(activity);
    if (activityClass == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    jmethodID getClassLoader =
        env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (getClassLoader == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    jobject classLoader = env->CallObjectMethod(activity, getClassLoader);
    if (classLoader == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
    if (classLoaderClass == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    jmethodID loadClass =
        env->GetMethodID(classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (loadClass == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    jstring className = make_string(env, name);
    if (className == nullptr) {
        return nullptr;
    }
    auto* loaded = static_cast<jclass>(env->CallObjectMethod(classLoader, loadClass, className));
    if (loaded == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    return loaded;
}

}  // namespace borealis::jni
