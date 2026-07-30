#include <jni.h>
#include <android/native_window_jni.h>

#include <string>

#include "native_self_test.h"
#include "putorana/graphics/Device.h"
#include "putorana/graphics/Frame.h"
#include "putorana/graphics/Instance.h"
#include "vulkan_check.h"

namespace {

#if defined(APP_ENABLE_VALIDATION)
constexpr bool kEnableValidation = true;
#else
constexpr bool kEnableValidation = false;
#endif

} // namespace

// Runs the moment System.loadLibrary("arreconstructor") returns. The VkInstance
// is created here because its lifetime is exactly the lifetime of this loaded
// library, unlike the device and the swapchain, which follow the Android surface
// and get created and destroyed many times over a single library load.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    putorana::graphics::instance_holder::CreateInstance(kEnableValidation);
    // Deliberately not JNI_ERR when creation fails: that surfaces in Kotlin as a
    // bare UnsatisfiedLinkError and throws away the reason. The reason is
    // reported through vulkanInstanceReport() instead, so the UI can show it.
    return JNI_VERSION_1_6;
}

// Android does not unload native libraries in practice, so this is here for
// correctness rather than because it is expected to run.
extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/) {
    putorana::graphics::instance_holder::DestroyInstance();
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_dongeronimo_arreconstructor_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}

// Reports what JNI_OnLoad managed to build. Reuses NativeSelfTestResult purely as
// an (ok, report) pair for crossing the JNI boundary.
extern "C" JNIEXPORT jobject JNICALL
Java_dev_dongeronimo_arreconstructor_MainActivity_vulkanInstanceReport(
        JNIEnv* env,
        jobject /* this */) {
    const putorana::graphics::Instance* instance = putorana::graphics::instance_holder::Get();

    SelfTestResult result;
    result.ok = instance != nullptr;
    result.report = result.ok ? instance->Describe()
                             : putorana::graphics::instance_holder::Error();
    return MakeNativeSelfTestResult(env, result);
}

// --- NativeRenderer -------------------------------------------------------
// The native entry points of the renderer, forwarded by RenderThread.kt. Unlike
// everything above, these do NOT run on the UI thread: RenderThread hops them
// onto the render thread first, and they all run on that same one thread.

extern "C" JNIEXPORT void JNICALL
Java_dev_dongeronimo_arreconstructor_NativeRenderer_surfaceCreated(
        JNIEnv* env,
        jobject /* this */,
        jobject surface) {
    // ANativeWindow_fromSurface takes its own reference on the window, so it
    // stays alive even if Kotlin drops the Surface object right after this
    // returns. Device takes ownership of that reference and is what releases it,
    // in OnSurfaceDestroyed.
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    putorana::graphics::device_holder::Get().OnSurfaceCreated(window);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_dongeronimo_arreconstructor_NativeRenderer_surfaceChanged(
        JNIEnv* /* env */,
        jobject /* this */,
        jint /* format */,
        jint width,
        jint height) {
    putorana::graphics::device_holder::Get().OnSurfaceChanged(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_dongeronimo_arreconstructor_NativeRenderer_surfaceDestroyed(
        JNIEnv* /* env */,
        jobject /* this */) {
    putorana::graphics::device_holder::Get().OnSurfaceDestroyed();
    putorana::graphics::ResetFrameStats();
}

extern "C" JNIEXPORT void JNICALL
Java_dev_dongeronimo_arreconstructor_NativeRenderer_drawFrame(
        JNIEnv* /* env */,
        jobject /* this */,
        jlong frameTimeNanos) {
    putorana::graphics::DrawFrame(frameTimeNanos);
}

// Not called by the UI. It is the "is anything working at all?" button: it runs
// on the application's instance, so it never creates a second one.
extern "C" JNIEXPORT jobject JNICALL
Java_dev_dongeronimo_arreconstructor_MainActivity_runVulkanSelfTest(
        JNIEnv* env,
        jobject /* this */) {
    const putorana::graphics::Instance* instance = putorana::graphics::instance_holder::Get();
    const VkInstance handle = instance != nullptr ? instance->handle() : VK_NULL_HANDLE;
    return MakeNativeSelfTestResult(env, RunVulkanVmaSelfTest(handle));
}
