#include "borealis/http.hpp"

#include "../http_internal.hpp"
#include "../jni_internal.hpp"

#include <jni.h>

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace borealis::http {
namespace {

constexpr int JavaErrorNone = 0;
constexpr int JavaErrorInvalidUrl = 1;
constexpr int JavaErrorUnsupportedScheme = 2;
constexpr int JavaErrorTimeout = 3;
constexpr int JavaErrorCanceled = 4;

int timeout_ms(std::chrono::milliseconds timeout) {
    const auto count = std::max<std::chrono::milliseconds::rep>(1, timeout.count());
    return static_cast<int>(
        std::min<std::chrono::milliseconds::rep>(count, std::numeric_limits<int>::max()));
}

jlong total_timeout_ms(const detail::Deadline& deadline) {
    const auto remaining = deadline.remaining_total();
    if (!remaining) {
        return 0;
    }
    return static_cast<jlong>(std::min<std::chrono::milliseconds::rep>(
        remaining->count(), std::numeric_limits<jlong>::max()));
}

Error map_java_error(int error) {
    switch (error) {
    case JavaErrorNone:
        return Error::None;
    case JavaErrorInvalidUrl:
        return Error::InvalidUrl;
    case JavaErrorUnsupportedScheme:
        return Error::UnsupportedScheme;
    case JavaErrorTimeout:
        return Error::Timeout;
    case JavaErrorCanceled:
        return Error::Canceled;
    default:
        return Error::Network;
    }
}

jobjectArray make_string_array(JNIEnv* env, const std::vector<Header>& headers, bool names) {
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr || jni::clear_pending_exception(env)) {
        return nullptr;
    }

    jobjectArray array =
        env->NewObjectArray(static_cast<jsize>(headers.size()), stringClass, nullptr);
    if (array == nullptr || jni::clear_pending_exception(env)) {
        return nullptr;
    }

    for (jsize i = 0; i < static_cast<jsize>(headers.size()); ++i) {
        const std::string& value =
            names ? headers[static_cast<size_t>(i)].name : headers[static_cast<size_t>(i)].value;
        jstring javaValue = jni::make_string(env, value);
        if (javaValue == nullptr) {
            return nullptr;
        }
        env->SetObjectArrayElement(array, i, javaValue);
        env->DeleteLocalRef(javaValue);
        if (jni::clear_pending_exception(env)) {
            return nullptr;
        }
    }
    return array;
}

std::vector<Header> read_headers(JNIEnv* env, jobjectArray names, jobjectArray values) {
    std::vector<Header> headers;
    if (names == nullptr || values == nullptr) {
        return headers;
    }

    const jsize count = std::min(env->GetArrayLength(names), env->GetArrayLength(values));
    headers.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto* name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        auto* value = static_cast<jstring>(env->GetObjectArrayElement(values, i));
        if (jni::clear_pending_exception(env)) {
            return {};
        }

        if (name != nullptr) {
            headers.push_back({
                .name = jni::to_string(env, name),
                .value = jni::to_string(env, value),
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

jbyteArray make_byte_array(JNIEnv* env, std::string_view value) {
    if (value.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(value.size()));
    if (result == nullptr || jni::clear_pending_exception(env)) {
        return nullptr;
    }
    if (!value.empty()) {
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(value.size()),
            reinterpret_cast<const jbyte*>(value.data()));
        if (jni::clear_pending_exception(env)) {
            return nullptr;
        }
    }
    return result;
}

detail::TransportResult result_from_response(JNIEnv* env, jobject response) {
    if (response == nullptr) {
        return {
            .error = Error::Network,
            .message = "Android HTTP request did not return a response",
        };
    }

    jclass responseClass = env->GetObjectClass(response);
    if (responseClass == nullptr || jni::clear_pending_exception(env)) {
        return {
            .error = Error::Network,
            .message = "Failed to inspect Android HTTP response",
        };
    }

    jfieldID errorField = env->GetFieldID(responseClass, "error", "I");
    jfieldID messageField = env->GetFieldID(responseClass, "message", "Ljava/lang/String;");
    if (errorField == nullptr || messageField == nullptr || jni::clear_pending_exception(env)) {
        return {
            .error = Error::Network,
            .message = "Android HTTP response shape was not recognized",
        };
    }

    const int javaError = env->GetIntField(response, errorField);
    auto* message = static_cast<jstring>(env->GetObjectField(response, messageField));
    if (jni::clear_pending_exception(env)) {
        return {
            .error = Error::Network,
            .message = "Failed to read Android HTTP response",
        };
    }
    return {
        .error = map_java_error(javaError),
        .message = jni::to_string(env, message),
    };
}

}  // namespace

bool available() noexcept {
    return true;
}

Backend backend() noexcept {
    return Backend::Android;
}

const char* backend_name() noexcept {
    return "Android";
}

detail::TransportResult detail::send_request(const TransportRequest& request) {
    auto* env = jni::env();
    jni::LocalFrame frame{env};
    if (!frame) {
        return {
            .error = Error::Network,
            .message = "Failed to access Android JNI environment",
        };
    }

    jclass clientClass = jni::find_app_class(env, "dev.encounter.borealis.BorealisHttpClient");
    if (clientClass == nullptr) {
        return {
            .error = Error::Network,
            .message = "Failed to load Android HTTP helper",
        };
    }

    jmethodID requestMethod = env->GetStaticMethodID(clientClass, "request",
        "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[BIIJJJ)"
        "Ldev/encounter/borealis/BorealisHttpClient$Response;");
    if (requestMethod == nullptr || jni::clear_pending_exception(env)) {
        return {
            .error = Error::Network,
            .message = "Failed to find Android HTTP helper method",
        };
    }

    jstring method = jni::make_string(env, detail::method_name(request.method));
    jstring url = jni::make_string(env, request.url);
    jobjectArray headerNames = make_string_array(env, request.headers, true);
    jobjectArray headerValues = make_string_array(env, request.headers, false);
    jbyteArray body = make_byte_array(env, detail::method_has_request_body(request.method) ?
                                               std::string_view{request.body} :
                                               std::string_view{});
    if (method == nullptr || url == nullptr || headerNames == nullptr || headerValues == nullptr ||
        body == nullptr)
    {
        return {
            .error = Error::Network,
            .message = "Failed to prepare Android HTTP request",
        };
    }

    jobject response = env->CallStaticObjectMethod(clientClass, requestMethod, method, url,
        headerNames, headerValues, body, timeout_ms(request.deadline->connect_timeout()),
        timeout_ms(request.deadline->idle_timeout()), total_timeout_ms(*request.deadline),
        static_cast<jlong>(reinterpret_cast<std::uintptr_t>(request.observer)),
        static_cast<jlong>(reinterpret_cast<std::uintptr_t>(request.signals)));
    if (jni::clear_pending_exception(env)) {
        return {
            .error = Error::Network,
            .message = "Android HTTP request failed with a Java exception",
        };
    }

    return result_from_response(env, response);
}

}  // namespace borealis::http

extern "C" JNIEXPORT jboolean JNICALL Java_dev_encounter_borealis_BorealisHttpClient_onResponse(
    JNIEnv* env, jclass, jlong observerAddress, jint statusCode, jobjectArray headerNames,
    jobjectArray headerValues) {
    try {
        auto* observer = reinterpret_cast<borealis::http::detail::TransportObserver*>(
            static_cast<std::uintptr_t>(observerAddress));
        if (observer == nullptr) {
            return JNI_TRUE;
        }
        std::vector<borealis::http::Header> headers =
            borealis::http::read_headers(env, headerNames, headerValues);
        return observer->on_response(static_cast<int>(statusCode), std::move(headers)) ==
                       borealis::http::detail::TransportObserver::Directive::Abort ?
                   JNI_TRUE :
                   JNI_FALSE;
    } catch (...) {
        return JNI_TRUE;
    }
}

extern "C" JNIEXPORT jboolean JNICALL Java_dev_encounter_borealis_BorealisHttpClient_onData(
    JNIEnv* env, jclass, jlong observerAddress, jbyteArray data, jint length) {
    try {
        auto* observer = reinterpret_cast<borealis::http::detail::TransportObserver*>(
            static_cast<std::uintptr_t>(observerAddress));
        if (observer == nullptr || data == nullptr || length < 0 ||
            length > env->GetArrayLength(data))
        {
            return JNI_TRUE;
        }

        std::vector<std::byte> bytes(static_cast<size_t>(length));
        if (length > 0) {
            env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
            if (borealis::jni::clear_pending_exception(env)) {
                return JNI_TRUE;
            }
        }
        return observer->on_data(bytes) ==
                       borealis::http::detail::TransportObserver::Directive::Abort ?
                   JNI_TRUE :
                   JNI_FALSE;
    } catch (...) {
        return JNI_TRUE;
    }
}

extern "C" JNIEXPORT jboolean JNICALL Java_dev_encounter_borealis_BorealisHttpClient_isCanceled(
    JNIEnv*, jclass, jlong signalsAddress) {
    auto* signals = reinterpret_cast<borealis::detail::TaskSignals*>(
        static_cast<std::uintptr_t>(signalsAddress));
    return signals != nullptr && signals->cancelRequested.load(std::memory_order_relaxed) ?
               JNI_TRUE :
               JNI_FALSE;
}
