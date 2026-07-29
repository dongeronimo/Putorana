#pragma once

#include <jni.h>

#include <string>

// Shared result type for native self-tests that are exposed to Kotlin as
// dev.dongeronimo.arreconstructor.NativeSelfTestResult.
struct SelfTestResult {
    // True only when every checked step succeeded.
    bool ok = false;
    // Human-readable description of what succeeded, or the first failing step.
    std::string report;
};

// Builds the Kotlin NativeSelfTestResult(ok, report) from a SelfTestResult and
// logs the report to logcat (tag "ARReconstructor"): INFO when ok, ERROR otherwise.
jobject MakeNativeSelfTestResult(JNIEnv* env, const SelfTestResult& result);
