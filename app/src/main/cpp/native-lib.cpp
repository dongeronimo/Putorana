#include <jni.h>
#include <android/native_window_jni.h>

#include <string>
#include <vector>

#include "native_self_test.h"
#include "putorana/Assets.h"
#include "putorana/ar/Subsystem.h"
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
    putorana::ar::subsystem_holder::Destroy();
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

// Hands over the AssetManager. Separate from JNI_OnLoad because there is no
// Context there to get one from, and it has to arrive before anything tries to
// load — see Assets.h on why the reference is kept global.
extern "C" JNIEXPORT void JNICALL
Java_dev_dongeronimo_arreconstructor_NativeRenderer_setAssetManager(
        JNIEnv* env,
        jobject /* this */,
        jobject assetManager) {
    putorana::assets::Attach(env, assetManager);
}

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

// --- NativeProfiler ---------------------------------------------------------

// Reads the GPU timings. UI thread, unlike everything under NativeRenderer:
// GpuProfiler::Snapshot takes a lock so that this can be, because the whole
// point is to read while the render thread keeps rendering.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_dev_dongeronimo_arreconstructor_NativeProfiler_passTimings(
        JNIEnv* env,
        jobject /* this */) {
    jclass timingClass = env->FindClass("dev/dongeronimo/arreconstructor/PassTiming");
    jmethodID constructor = env->GetMethodID(timingClass, "<init>", "(Ljava/lang/String;F)V");

    putorana::graphics::Device& device = putorana::graphics::device_holder::Get();
    // HasSurface is the guard, not a null check on the profiler: the profiler is
    // destroyed with the device every time the app is backgrounded, and this can
    // legitimately be called in that window.
    std::vector<putorana::graphics::PassTiming> timings;
    if (device.HasSurface()) {
        timings = device.profiler().Snapshot();
    }

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(timings.size()), timingClass,
                                              nullptr);
    for (size_t i = 0; i < timings.size(); ++i) {
        jstring name = env->NewStringUTF(timings[i].name.c_str());
        jobject timing = env->NewObject(timingClass, constructor, name,
                                        static_cast<jfloat>(timings[i].milliseconds));
        env->SetObjectArrayElement(result, static_cast<jsize>(i), timing);
        // The local reference table is small and this runs several times a
        // second. Two objects per entry left behind would overflow it long
        // before the frame loop noticed.
        env->DeleteLocalRef(timing);
        env->DeleteLocalRef(name);
    }
    return result;
}

// --- NativeAr ---------------------------------------------------------------
// The AR subsystem's entry points. Unlike NativeRenderer these run on the UI
// thread, driven by the activity's lifecycle, while Update runs on the render
// thread — putorana::ar::Subsystem takes a lock precisely because of that split.

// Creates the ArSession. Separate from JNI_OnLoad, which is where the VkInstance
// is built, because this one needs a Context and a granted camera permission,
// and neither exists when the library loads.
extern "C" JNIEXPORT jobject JNICALL
Java_dev_dongeronimo_arreconstructor_NativeAr_nativeCreate(
        JNIEnv* env,
        jobject /* this */,
        jobject context) {
    SelfTestResult result;
    std::string error;
    result.ok = putorana::ar::subsystem_holder::Create(env, context, error);
    result.report = result.ok ? "AR session created" : error;
    return MakeNativeSelfTestResult(env, result);
}

// Starts the camera. Fails, rather than prompting, when the permission is not
// granted — asking is the activity's job and has to have happened already.
extern "C" JNIEXPORT jobject JNICALL
Java_dev_dongeronimo_arreconstructor_NativeAr_resume(
        JNIEnv* env,
        jobject /* this */) {
    SelfTestResult result;
    putorana::ar::Subsystem* subsystem = putorana::ar::subsystem_holder::Get();
    if (subsystem == nullptr) {
        result.report = "no AR session";
        return MakeNativeSelfTestResult(env, result);
    }
    std::string error;
    result.ok = subsystem->Resume(error);
    result.report = result.ok ? "AR session resumed" : error;
    return MakeNativeSelfTestResult(env, result);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_dongeronimo_arreconstructor_NativeAr_pause(
        JNIEnv* /* env */,
        jobject /* this */) {
    if (putorana::ar::Subsystem* subsystem = putorana::ar::subsystem_holder::Get()) {
        subsystem->Pause();
    }
}

// The display rotation, not the swapchain's preTransform. Everything ARCore
// reports in view coordinates is wrong until this has been told the truth, which
// includes the UVs the camera background is drawn with.
extern "C" JNIEXPORT void JNICALL
Java_dev_dongeronimo_arreconstructor_NativeAr_nativeSetDisplayGeometry(
        JNIEnv* /* env */,
        jobject /* this */,
        jint rotation,
        jint width,
        jint height) {
    if (putorana::ar::Subsystem* subsystem = putorana::ar::subsystem_holder::Get()) {
        subsystem->SetDisplayGeometry(rotation, width, height);
    }
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
