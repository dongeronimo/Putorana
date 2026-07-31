#include "Assets.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

namespace putorana::assets {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * The native handle, and the Java object it is only valid alongside. They are
 * always written together — see the note in Assets.h about why the reference
 * has to be global.
 * */
AAssetManager* g_manager = nullptr;
jobject g_javaManager = nullptr;

} // namespace

void Attach(JNIEnv* env, jobject assetManager) {
    if (g_javaManager != nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "assets: Attach called twice; releasing the previous manager");
        Detach(env);
    }
    // The global reference first, and the native pointer FROM it: deriving the
    // pointer from the local reference and then keeping only a global one is a
    // subtle way to end up with a pointer into an object the collector may still
    // move or free.
    g_javaManager = env->NewGlobalRef(assetManager);
    g_manager = AAssetManager_fromJava(env, g_javaManager);
    if (g_manager == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "AAssetManager_fromJava returned null; nothing can be loaded");
        env->DeleteGlobalRef(g_javaManager);
        g_javaManager = nullptr;
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "assets: ready");
}

void Detach(JNIEnv* env) {
    if (g_javaManager != nullptr) {
        env->DeleteGlobalRef(g_javaManager);
        g_javaManager = nullptr;
    }
    g_manager = nullptr;
}

bool Ready() {
    return g_manager != nullptr;
}

std::vector<uint8_t> Read(const std::string& path, std::string& error) {
    if (g_manager == nullptr) {
        error = "assets not attached; nothing can read '" + path + "'";
        return {};
    }

    // BUFFER mode lets the platform mmap the asset when it is stored
    // uncompressed, which for a .glb of any size is the difference between a
    // page mapping and a full decompress into the heap.
    AAsset* asset = AAssetManager_open(g_manager, path.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        error = "asset not found: '" + path + "'";
        return {};
    }

    const off64_t length = AAsset_getLength64(asset);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    const int read = AAsset_read(asset, bytes.data(), bytes.size());
    AAsset_close(asset);

    if (read < 0 || static_cast<size_t>(read) != bytes.size()) {
        error = "short read on '" + path + "': got " + std::to_string(read) + " of " +
                std::to_string(bytes.size()) + " bytes";
        return {};
    }
    return bytes;
}

} // namespace putorana::assets
