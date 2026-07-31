#ifndef PUTORANA_ASSETS_H
#define PUTORANA_ASSETS_H

#include <jni.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * Reading files out of the APK.
 *
 * An Android app's assets are not files on a filesystem: they live inside the
 * APK, usually compressed, and the only way at them is AAssetManager. There is
 * no path that fopen would accept, which is why every loader in this project
 * takes bytes rather than a filename.
 *
 * Free functions in a namespace, the same shape as instance_holder and
 * device_holder, and for the same reason: JNI entry points are free functions
 * with no object to hang state off.
 * */
namespace putorana::assets {

/**
 * Hands over the Java AssetManager. Called once from JNI, early — before
 * anything tries to load.
 *
 * Takes a global reference to the Java object and holds it, which is not
 * optional: AAssetManager_fromJava returns a pointer whose validity is tied to
 * the Java object staying alive, and a local reference dies when the JNI call
 * returns. Skipping this is the classic version of this bug — it works for a
 * while, then the collector runs.
 * */
void Attach(JNIEnv* env, jobject assetManager);

/** Releases the global reference. Nothing may read assets afterwards. */
void Detach(JNIEnv* env);

bool Ready();

/**
 * The whole file at `path`, relative to the assets root. Empty vector with the
 * reason in `error` when it is missing or unreadable.
 *
 * Whole file and not a stream because everything read here — a .glb, a compiled
 * shader — is consumed in one go by a parser that wants a contiguous buffer, and
 * because an asset inside an APK may be compressed, which makes seeking
 * expensive rather than free.
 * */
std::vector<uint8_t> Read(const std::string& path, std::string& error);

} // namespace putorana::assets

#endif //PUTORANA_ASSETS_H
