#include <jni.h>
#include <string>

#include "native_self_test.h"
#include "vulkan_check.h"

extern "C" JNIEXPORT jstring JNICALL
Java_dev_dongeronimo_arreconstructor_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jobject JNICALL
Java_dev_dongeronimo_arreconstructor_MainActivity_runVulkanSelfTest(
        JNIEnv* env,
        jobject /* this */) {
    return MakeNativeSelfTestResult(env, RunVulkanVmaSelfTest());
}
