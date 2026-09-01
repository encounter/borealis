#include <borealis/presentation.hpp>

#include <cmath>

#if defined(__ANDROID__)
#include "jni_internal.hpp"

#include <jni.h>
#endif

namespace borealis::presentation {

bool set_preferred_frame_rate(float framesPerSecond) noexcept {
    if (!std::isfinite(framesPerSecond) || framesPerSecond < 0.0f) {
        return false;
    }

#if defined(__ANDROID__)
    jni::LocalFrame frame{jni::env()};
    if (!frame) {
        return false;
    }
    auto method = jni::activity_method("setPreferredSurfaceFrameRate", "(F)V");
    if (!method) {
        return false;
    }
    jvalue args[1]{};
    args[0].f = framesPerSecond;
    method.env->CallVoidMethodA(method.activity, method.method, args);
    return !jni::clear_pending_exception(method.env);
#else
    (void)framesPerSecond;
    return false;
#endif
}

}  // namespace borealis::presentation
