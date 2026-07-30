#include <jni.h>
#include <string>

#include "native_self_test.h"
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
