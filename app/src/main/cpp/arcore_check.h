#pragma once

#include <jni.h>

#include "native_self_test.h"

// Sanity check for the ARCore plumbing, in the same spirit as RunVulkanVmaSelfTest:
// it answers "is the ARCore half of this build actually wired up?" without needing
// a session, a camera, or a surface.
//
// It asks ARCore whether it is available on this device, which is the one native
// call that needs nothing but a JNIEnv and a Context. Reaching an answer at all --
// even an unsupported-device answer -- proves the whole chain: the vendored
// arcore_c_api.h matches the linked libarcore_sdk_c.so, the loader found that .so
// inside the APK, and the ARCore APK is reachable over the package manager.
//
// A false result is not necessarily a broken build: a device without ARCore
// support, or without the ARCore APK installed, fails here exactly as it should.
// The report says which of the two it was.
SelfTestResult RunArcoreSelfTest(JNIEnv* env, jobject context);
