#include "../jni_internal.hpp"
#include "../ws_internal.hpp"

#include "borealis/log.hpp"

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr borealis::Log Log{"borealis::ws"};
}

namespace borealis::ws::detail {
namespace {

struct CallbackState {
    std::mutex mutex;
    std::shared_ptr<EventSink> sink;
    bool active = true;
};

std::mutex g_callbacksMutex;
std::unordered_map<jlong, std::shared_ptr<CallbackState>> g_callbacks;
std::atomic<jlong> g_nextToken = 1;

std::shared_ptr<CallbackState> callback_state(jlong token) {
    std::lock_guard lock{g_callbacksMutex};
    const auto found = g_callbacks.find(token);
    return found != g_callbacks.end() ? found->second : nullptr;
}

void remove_callback(jlong token) {
    std::lock_guard lock{g_callbacksMutex};
    g_callbacks.erase(token);
}

struct JavaApi {
    jni::AppClass client{"dev.encounter.borealis.BorealisWebSocketClient"};
    jni::StaticMethod connect{
        client,
        "connect",
        "(JLjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;II)Z",
    };
    jni::StaticMethod send{client, "send", "(JI[B)Z"};
    jni::StaticMethod close{client, "close", "(JILjava/lang/String;)V"};
    jni::StaticMethod abort{client, "abort", "(J)V"};
};

JavaApi& java_api() {
    static JavaApi value;
    return value;
}

class AndroidTransport final : public Transport {
public:
    ~AndroidTransport() override { abort(); }

    void start(const Options& options, std::shared_ptr<EventSink> sink) override {
        token = g_nextToken.fetch_add(1, std::memory_order_relaxed);
        state = std::make_shared<CallbackState>();
        state->sink = std::move(sink);
        {
            std::lock_guard lock{g_callbacksMutex};
            g_callbacks.emplace(token, state);
        }

        JNIEnv* env = jni::env();
        jni::LocalFrame frame{env};
        if (!frame) {
            fail_start("Failed to access the Android JNI environment");
            return;
        }
        auto& api = java_api();
        jclass client = api.client.get(env);
        jmethodID connect = api.connect.get(env);
        if (client == nullptr || connect == nullptr) {
            fail_start("Failed to find the Android WebSocket helper");
            return;
        }
        jstring url = jni::make_string(env, options.url);
        jobjectArray headerNames = jni::make_header_array(env, options.headers, true);
        jobjectArray headerValues = jni::make_header_array(env, options.headers, false);
        if (url == nullptr || headerNames == nullptr || headerValues == nullptr) {
            fail_start("Failed to prepare the Android WebSocket request");
            return;
        }
        const jboolean started = env->CallStaticBooleanMethod(client, connect, token, url,
            headerNames, headerValues, jni::timeout_ms(options.connectTimeout),
            options.keepaliveInterval.count() != 0 ? jni::timeout_ms(options.keepaliveInterval) :
                                                     0);
        if (jni::clear_pending_exception(env) || started != JNI_TRUE) {
            fail_start("Android could not start the WebSocket connection");
        }
    }

    bool send(MessageKind kind, std::string data) override {
        JNIEnv* env = jni::env();
        jni::LocalFrame frame{env};
        if (!frame || token == 0) {
            return false;
        }
        auto& api = java_api();
        jclass client = api.client.get(env);
        jmethodID method = api.send.get(env);
        jbyteArray bytes = jni::make_byte_array(env, data);
        if (method == nullptr || bytes == nullptr || jni::clear_pending_exception(env)) {
            return false;
        }
        const jboolean accepted = env->CallStaticBooleanMethod(
            client, method, token, kind == MessageKind::Text ? 0 : 1, bytes);
        return !jni::clear_pending_exception(env) && accepted == JNI_TRUE;
    }

    void close(uint16_t code, std::string reason) override {
        call_close(static_cast<jint>(code), reason);
    }

    void abort() noexcept override try {
        if (token == 0) {
            return;
        }
        const jlong currentToken = std::exchange(token, 0);
        if (state) {
            std::lock_guard lock{state->mutex};
            state->active = false;
            state->sink.reset();
        }
        remove_callback(currentToken);

        JNIEnv* env = jni::env();
        jni::LocalFrame frame{env};
        if (!frame) {
            return;
        }
        auto& api = java_api();
        jclass client = api.client.get(env);
        jmethodID method = api.abort.get(env);
        if (client != nullptr && method != nullptr) {
            env->CallStaticVoidMethod(client, method, currentToken);
            jni::clear_pending_exception(env);
        }
    }
    BOREALIS_CATCH()

private:
    void fail_start(std::string message) {
        std::shared_ptr<EventSink> sink;
        if (state) {
            std::lock_guard lock{state->mutex};
            sink = state->sink;
            state->active = false;
        }
        remove_callback(token);
        token = 0;
        if (sink) {
            sink->closed(Error::Network, std::move(message), 0, 0, {});
        }
    }

    void call_close(jint code, std::string_view reason) {
        JNIEnv* env = jni::env();
        jni::LocalFrame frame{env};
        if (!frame || token == 0) {
            return;
        }
        auto& api = java_api();
        jclass client = api.client.get(env);
        jmethodID method = api.close.get(env);
        jstring reasonText = jni::make_string(env, reason);
        if (client != nullptr && method != nullptr && reasonText != nullptr) {
            env->CallStaticVoidMethod(client, method, token, code, reasonText);
            jni::clear_pending_exception(env);
        }
    }

    jlong token = 0;
    std::shared_ptr<CallbackState> state;
};

}  // namespace

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<AndroidTransport>();
}

bool backend_available() noexcept {
    return true;
}

}  // namespace borealis::ws::detail

extern "C" JNIEXPORT void JNICALL Java_dev_encounter_borealis_BorealisWebSocketClient_nativeOnOpen(
    JNIEnv* env, jclass, jlong token, jstring protocol, jobjectArray headerNames,
    jobjectArray headerValues) try {
    auto state = borealis::ws::detail::callback_state(token);
    if (!state) {
        return;
    }
    std::lock_guard lock{state->mutex};
    if (state->active && state->sink) {
        state->sink->opened(borealis::jni::to_string(env, protocol),
            borealis::jni::read_headers(env, headerNames, headerValues));
    }
}
BOREALIS_CATCH()

extern "C" JNIEXPORT void JNICALL
Java_dev_encounter_borealis_BorealisWebSocketClient_nativeOnMessage(
    JNIEnv* env, jclass, jlong token, jint kind, jbyteArray data) try {
    auto state = borealis::ws::detail::callback_state(token);
    if (!state || data == nullptr) {
        return;
    }
    const jsize size = env->GetArrayLength(data);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (size != 0) {
        env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
        if (borealis::jni::clear_pending_exception(env)) {
            return;
        }
    }
    std::lock_guard lock{state->mutex};
    if (state->active && state->sink) {
        state->sink->message(
            kind == 0 ? borealis::ws::MessageKind::Text : borealis::ws::MessageKind::Binary,
            std::move(bytes));
    }
}
BOREALIS_CATCH()

extern "C" JNIEXPORT void JNICALL
Java_dev_encounter_borealis_BorealisWebSocketClient_nativeOnSendComplete(
    JNIEnv*, jclass, jlong token, jlong bytes) try {
    auto state = borealis::ws::detail::callback_state(token);
    if (!state) {
        return;
    }
    std::lock_guard lock{state->mutex};
    if (state->active && state->sink) {
        state->sink->send_complete(
            static_cast<size_t>(std::max<jlong>(bytes, 0)), borealis::ws::Error::None, {});
    }
}
BOREALIS_CATCH()

extern "C" JNIEXPORT void JNICALL
Java_dev_encounter_borealis_BorealisWebSocketClient_nativeOnClosed(
    JNIEnv* env, jclass, jlong token, jint code, jstring reason) try {
    auto state = borealis::ws::detail::callback_state(token);
    if (!state) {
        return;
    }
    {
        std::lock_guard lock{state->mutex};
        if (!state->active || !state->sink) {
            return;
        }
        state->sink->closed(borealis::ws::Error::None, {}, 0, static_cast<uint16_t>(code),
            borealis::jni::to_string(env, reason));
        state->active = false;
    }
    borealis::ws::detail::remove_callback(token);
}
BOREALIS_CATCH()

extern "C" JNIEXPORT void JNICALL
Java_dev_encounter_borealis_BorealisWebSocketClient_nativeOnFailure(JNIEnv* env, jclass,
    jlong token, jstring message, jint status, jobjectArray headerNames,
    jobjectArray headerValues) try {
    auto state = borealis::ws::detail::callback_state(token);
    if (!state) {
        return;
    }
    {
        std::lock_guard lock{state->mutex};
        if (!state->active || !state->sink) {
            return;
        }
        state->sink->closed(
            status != 0 ? borealis::ws::Error::Handshake : borealis::ws::Error::Network,
            borealis::jni::to_string(env, message), status, 0, {},
            borealis::jni::read_headers(env, headerNames, headerValues));
        state->active = false;
    }
    borealis::ws::detail::remove_callback(token);
}
BOREALIS_CATCH()
