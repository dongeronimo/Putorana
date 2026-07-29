#include "native_self_test.h"

#include <android/log.h>

jobject MakeNativeSelfTestResult(JNIEnv* env, const SelfTestResult& result) {
    __android_log_print(
            result.ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
            "ARReconstructor", "%s", result.report.c_str());

    jclass resultClass =
            env->FindClass("dev/dongeronimo/arreconstructor/NativeSelfTestResult");
    jmethodID constructor =
            env->GetMethodID(resultClass, "<init>", "(ZLjava/lang/String;)V");
    jstring report = env->NewStringUTF(result.report.c_str());

    return env->NewObject(
            resultClass, constructor,
            result.ok ? JNI_TRUE : JNI_FALSE, report);
}
