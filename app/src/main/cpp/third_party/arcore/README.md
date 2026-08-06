# ARCore C API, vendored header

`include/arcore_c_api.h`, version **1.54.0**, taken verbatim from

    https://raw.githubusercontent.com/google-ar/arcore-android-sdk/v1.54.0/libraries/include/arcore_c_api.h

## Why it is vendored instead of coming from the AAR

The native side of ARCore is split across two artifacts that are published
separately, and only one of them is on Maven:

  * the **library**, `libarcore_sdk_c.so`, ships inside the AAR
    `com.google.ar:core:1.54.0`, under `jni/<abi>/`;
  * the **header** is *not in the AAR at all*. Unzipping the AAR (or its
    `classes.jar`) yields `AndroidManifest.xml`, `classes.jar`, `jni/`, `res/`,
    `proguard.txt` and `R.txt`: no `include/`, no `.h`. The header exists only
    in the `google-ar/arcore-android-sdk` GitHub repository, tagged per release.

So there is nothing to extract, and no Maven coordinate that would provide it.
Copying the file in is the supported route: the SDK's own C samples do the same,
reaching into a sibling checkout of that repo for `libraries/include`.

The `.so` is *not* vendored. `src/main/cpp/CMakeLists.txt` downloads the AAR and
unzips `jni/**` out of it at configure time, alongside the `FetchContent` call
that already pulls assimp, and points an `IMPORTED` target at the result. It has
to do this itself: AGP packages an AAR's `jni/` payload into the APK, but never
exposes its path to the native build, so there is nothing for
`IMPORTED_LOCATION` to point at otherwise.

The obvious alternative, a Gradle `Copy` task unzipping a resolved
configuration, which is what the SDK's samples do, was tried and dropped. It
forces Gradle to resolve that configuration while building the task graph, which
AGP flags as configuration-time resolution on *every* build, and it still leaves
the file missing during Android Studio sync, since sync runs the CMake configure
outside the task graph.

## Keeping the versions in sync

There are two things to line up on an upgrade, and only one of them is
automatic:

  * `arcore` in `gradle/libs.versions.toml`, the single source of truth. It
    feeds both `implementation(libs.arcore)` and the `-DARCORE_VERSION` handed to
    CMake, so the `.so` that gets linked against and the `.so` that gets packaged
    into the APK cannot drift apart.
  * **this header**, which nothing in the build checks. A mismatch against the
    version above links cleanly and then fails at runtime, on a missing symbol
    or, worse, on a struct whose layout moved.

So to upgrade, set `arcore = "X.Y.Z"` in the version catalog and re-fetch the
header at the matching tag:

    curl -L -o include/arcore_c_api.h \
      https://raw.githubusercontent.com/google-ar/arcore-android-sdk/vX.Y.Z/libraries/include/arcore_c_api.h

## License

The header is covered by the *ARCore Additional Terms of Service*
(https://developers.google.com/ar/develop/terms), not by this project's license.
The file's own copyright banner is left intact.
