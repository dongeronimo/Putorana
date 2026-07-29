package dev.dongeronimo.arreconstructor

/**
 * Result of a native self-test. Constructed from C++ via JNI, so the
 * constructor signature (ZLjava/lang/String;) must stay in sync with
 * MakeNativeSelfTestResult in native_self_test.cpp.
 */
data class NativeSelfTestResult(
    val ok: Boolean,
    val report: String,
)
