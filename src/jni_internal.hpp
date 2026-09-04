#pragma once

#include "borealis/http.hpp"

#include <SDL3/SDL_system.h>

#include <jni.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

inline int timeout_ms(std::chrono::milliseconds timeout) {
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(
        timeout.count(), 1, std::numeric_limits<int>::max()));
}

inline jobjectArray make_header_array(
    JNIEnv* env, const std::vector<http::Header>& headers, bool names) {
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    jobjectArray result =
        env->NewObjectArray(static_cast<jsize>(headers.size()), stringClass, nullptr);
    if (result == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    for (jsize index = 0; index < static_cast<jsize>(headers.size()); ++index) {
        const auto& header = headers[static_cast<size_t>(index)];
        jstring value = make_string(env, names ? header.name : header.value);
        if (value == nullptr) {
            return nullptr;
        }
        env->SetObjectArrayElement(result, index, value);
        env->DeleteLocalRef(value);
        if (clear_pending_exception(env)) {
            return nullptr;
        }
    }
    return result;
}

inline jbyteArray make_byte_array(JNIEnv* env, std::string_view value) {
    if (value.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(value.size()));
    if (result == nullptr || clear_pending_exception(env)) {
        return nullptr;
    }
    if (!value.empty()) {
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(value.size()),
            reinterpret_cast<const jbyte*>(value.data()));
        if (clear_pending_exception(env)) {
            return nullptr;
        }
    }
    return result;
}

inline std::vector<http::Header> read_headers(
    JNIEnv* env, jobjectArray names, jobjectArray values) {
    std::vector<http::Header> headers;
    if (names == nullptr || values == nullptr) {
        return headers;
    }
    const jsize count = std::min(env->GetArrayLength(names), env->GetArrayLength(values));
    headers.reserve(static_cast<size_t>(count));
    for (jsize index = 0; index < count; ++index) {
        auto* name = static_cast<jstring>(env->GetObjectArrayElement(names, index));
        auto* value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
        if (clear_pending_exception(env)) {
            return {};
        }
        if (name != nullptr) {
            headers.push_back({
                .name = to_string(env, name),
                .value = to_string(env, value),
            });
        }
        if (name != nullptr) {
            env->DeleteLocalRef(name);
        }
        if (value != nullptr) {
            env->DeleteLocalRef(value);
        }
    }
    return headers;
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

class AppClass {
public:
    explicit AppClass(const char* name) : m_name{name} {}

    jclass get(JNIEnv* env) {
        std::lock_guard lock{m_mutex};
        if (m_class != nullptr) {
            return m_class;
        }
        jclass local = find_app_class(env, m_name);
        if (local == nullptr) {
            return nullptr;
        }
        m_class = static_cast<jclass>(env->NewGlobalRef(local));
        if (clear_pending_exception(env)) {
            m_class = nullptr;
        }
        return m_class;
    }

private:
    const char* m_name;
    std::mutex m_mutex;
    jclass m_class = nullptr;
};

class StaticMethod {
public:
    StaticMethod(AppClass& owner, const char* name, const char* signature)
        : m_owner{owner}, m_name{name}, m_signature{signature} {}

    jmethodID get(JNIEnv* env) {
        std::lock_guard lock{m_mutex};
        if (m_method != nullptr) {
            return m_method;
        }
        jclass owner = m_owner.get(env);
        if (owner == nullptr) {
            return nullptr;
        }
        m_method = env->GetStaticMethodID(owner, m_name, m_signature);
        if (clear_pending_exception(env)) {
            m_method = nullptr;
        }
        return m_method;
    }

private:
    AppClass& m_owner;
    const char* m_name;
    const char* m_signature;
    std::mutex m_mutex;
    jmethodID m_method = nullptr;
};

}  // namespace borealis::jni
