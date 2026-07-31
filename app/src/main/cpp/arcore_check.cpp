#include "arcore_check.h"

#include "arcore_c_api.h"

namespace {

// AR_AVAILABILITY_UNKNOWN_CHECKING is the one answer that is not an answer:
// ArCoreApk_checkAvailability is documented to kick off an asynchronous query and
// return it while that query is in flight. It is reported as-is rather than
// polled, because for a self-test "ARCore is still deciding" is a perfectly good
// outcome -- the call went through, which is all this is asking.
const char* Describe(ArAvailability availability) {
    switch (availability) {
        case AR_AVAILABILITY_SUPPORTED_INSTALLED:
            return "ARCore supported and installed";
        case AR_AVAILABILITY_SUPPORTED_NOT_INSTALLED:
            return "ARCore supported, APK not installed";
        case AR_AVAILABILITY_SUPPORTED_APK_TOO_OLD:
            return "ARCore supported, installed APK too old";
        case AR_AVAILABILITY_UNSUPPORTED_DEVICE_NOT_CAPABLE:
            return "device does not support ARCore";
        case AR_AVAILABILITY_UNKNOWN_CHECKING:
            return "ARCore availability still being checked";
        case AR_AVAILABILITY_UNKNOWN_TIMED_OUT:
            return "ARCore availability check timed out";
        case AR_AVAILABILITY_UNKNOWN_ERROR:
            break;
    }
    return "ARCore availability unknown (internal error)";
}

} // namespace

SelfTestResult RunArcoreSelfTest(JNIEnv* env, jobject context) {
    SelfTestResult result;

    // The C API takes these as void*: it is deliberately free of any dependency on
    // jni.h, so the caller is the one that knows they are a JNIEnv and a jobject.
    ArAvailability availability = AR_AVAILABILITY_UNKNOWN_ERROR;
    ArCoreApk_checkAvailability(env, context, &availability);

    result.ok = availability == AR_AVAILABILITY_SUPPORTED_INSTALLED;
    result.report = Describe(availability);
    return result;
}
