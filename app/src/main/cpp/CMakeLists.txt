# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("arreconstructor")

include(FetchContent)
FetchContent_Declare(
        assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG v5.3.1
        GIT_SHALLOW TRUE
)
# Configure Assimp - minimal build
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLB_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_STL_EXPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_ANDROID_JNIIOSYSTEM OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
message(STATUS "Fetching dependencies...")
FetchContent_MakeAvailable(assimp)

find_library(jnigraphics-lib jnigraphics REQUIRED)
find_library(camera2ndk-lib camera2ndk REQUIRED)
find_library(mediandk-lib mediandk REQUIRED)

# ARCore. The header and the library come from different places and behave
# differently in this build. See app/src/main/cpp/third_party/arcore/README.md.
#
# The header is vendored, so it always exists and it is safe to require it: if it
# is missing, the clone is incomplete, and saying so now beats dumping hundreds of
# "unknown type ArSession" errors.
set(ARCORE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/arcore/include)
if(NOT EXISTS ${ARCORE_INCLUDE_DIR}/arcore_c_api.h)
    message(FATAL_ERROR
            "arcore_c_api.h is not in ${ARCORE_INCLUDE_DIR}. "
            "See third_party/arcore/README.md for how to get it.")
endif()

# The library comes from the Maven AAR. It is downloaded here, at configure time,
# for the same reason assimp arrives through FetchContent just above: this is the
# point where CMake knows what it needs and can still fail legibly.
#
# Why not leave this to Gradle, which already resolves the same AAR for the
# `implementation`? Because AGP packages an AAR's jni/ into the APK but never
# exposes that path to the native build: there is nothing for IMPORTED_LOCATION
# to point at. Unzipping it in a Gradle task (which is what the SDK's own C
# samples do) forces a configuration to be resolved while the task graph is being
# built, which AGP flags as configuration-time resolution on every build, and it
# would still leave the file missing during an Android Studio sync, since sync
# runs this configure outside the task graph.
#
# ARCORE_VERSION comes from the version catalog, via app/build.gradle.kts.
if(NOT DEFINED ARCORE_VERSION)
    message(FATAL_ERROR "ARCORE_VERSION was not defined; it should come from app/build.gradle.kts.")
endif()

set(ARCORE_AAR ${CMAKE_CURRENT_BINARY_DIR}/arcore/core-${ARCORE_VERSION}.aar)
set(ARCORE_UNPACKED ${CMAKE_CURRENT_BINARY_DIR}/arcore/unpacked)

# No URL_HASH on purpose: a fixed hash here would have to be updated on every
# version bump, in a different file from the one that decides the version, and the
# protection it buys is the same one Gradle already gives (or does not give) when
# it downloads exactly this artifact for the `implementation`: HTTPS to
# dl.google.com, with no dependency verification. Not worth a third place to sync.
if(NOT EXISTS ${ARCORE_UNPACKED}/jni/${ANDROID_ABI}/libarcore_sdk_c.so)
    message(STATUS "Fetching ARCore ${ARCORE_VERSION}...")
    file(DOWNLOAD
            "https://dl.google.com/dl/android/maven2/com/google/ar/core/${ARCORE_VERSION}/core-${ARCORE_VERSION}.aar"
            ${ARCORE_AAR}
            STATUS ARCORE_DOWNLOAD_STATUS
            TLS_VERIFY ON
            SHOW_PROGRESS)
    list(GET ARCORE_DOWNLOAD_STATUS 0 ARCORE_DOWNLOAD_CODE)
    if(NOT ARCORE_DOWNLOAD_CODE EQUAL 0)
        list(GET ARCORE_DOWNLOAD_STATUS 1 ARCORE_DOWNLOAD_ERROR)
        # The partial file has to go, or the EXISTS above takes it for a good one
        # on the next build and the error resurfaces further along, at link time.
        file(REMOVE ${ARCORE_AAR})
        message(FATAL_ERROR "Downloading the ARCore AAR failed: ${ARCORE_DOWNLOAD_ERROR}")
    endif()
    # ARCHIVE_EXTRACT recognises the zip by its content, not by the .aar extension.
    file(ARCHIVE_EXTRACT INPUT ${ARCORE_AAR} DESTINATION ${ARCORE_UNPACKED} PATTERNS "jni/*")
endif()

set(ARCORE_LIB ${ARCORE_UNPACKED}/jni/${ANDROID_ABI}/libarcore_sdk_c.so)
if(NOT EXISTS ${ARCORE_LIB})
    message(FATAL_ERROR "The ARCore AAR did not carry jni/${ANDROID_ABI}/libarcore_sdk_c.so.")
endif()

# IMPORTED rather than find_library: the .so is not in the NDK sysroot, it is a
# downloaded artifact at an absolute path that is already known here.
add_library(arcore::sdk_c SHARED IMPORTED)
set_target_properties(arcore::sdk_c PROPERTIES IMPORTED_LOCATION ${ARCORE_LIB})
# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
add_library(${CMAKE_PROJECT_NAME} SHARED
        arcore_check.cpp
        putorana/Assets.cpp
        putorana/ar/Subsystem.cpp
        putorana/behavours/BaseBehaviour.cpp
        putorana/behavours/RotationBehaviour.cpp
        putorana/graphics/Allocator.cpp
        putorana/graphics/Buffer.cpp
        putorana/graphics/ArCamera.cpp
        putorana/graphics/Camera.cpp
        putorana/graphics/CameraFeed.cpp
        putorana/graphics/ChunkMaterial.cpp
        putorana/graphics/DescriptorPool.cpp
        putorana/graphics/Device.cpp
        putorana/graphics/FinalPass.cpp
        putorana/graphics/FlatColorMaterial.cpp
        putorana/graphics/Frame.cpp
        putorana/graphics/FrameRing.cpp
        putorana/graphics/GltfLoader.cpp
        putorana/graphics/GpuProfiler.cpp
        putorana/graphics/Image.cpp
        putorana/graphics/Instance.cpp
        putorana/graphics/Light.cpp
        putorana/graphics/Material.cpp
        putorana/graphics/Mesh.cpp
        putorana/graphics/MeshPass.cpp
        putorana/graphics/Node.cpp
        putorana/graphics/PerspectiveCamera.cpp
        putorana/graphics/PhysicalDevice.cpp
        putorana/graphics/Renderable.cpp
        putorana/graphics/ShaderModule.cpp
        putorana/graphics/Swapchain.cpp
        putorana/graphics/World.cpp
        putorana/worlds/WorldRegistry.cpp
        putorana/worlds/helloWorld/HelloWorld.cpp
        putorana/worlds/openChiselWorld/OpenChiselWorld.cpp
        # volk's implementation unit: dlopen's libvulkan.so and defines the
        # global vk* function pointers that every other TU here resolves against.
        third_party/volk/volk.c
        vma_impl.cpp
        native_self_test.cpp
        vulkan_check.cpp
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        native-lib.cpp)

# No "d" on the end of the library name in Debug builds.
#
# assimp's CMakeLists does `SET(CMAKE_DEBUG_POSTFIX "d" CACHE STRING ...)`, and
# FetchContent_MakeAvailable runs it in THIS project's scope, before the
# add_library above. A target picks up its <CONFIG>_POSTFIX from the matching
# CMAKE_ variable at creation time, so ours silently became
# libarreconstructord.so while System.loadLibrary("arreconstructor") kept asking
# for the name without it: an UnsatisfiedLinkError at the first line of
# MainActivity, blaming a library that is right there in the APK.
#
# Set on the target rather than by clearing the cache variable, so assimp keeps
# its own convention (libassimpd.so) and this stays a statement about us.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        DEBUG_POSTFIX ""
        RELWITHDEBINFO_POSTFIX ""
        MINSIZEREL_POSTFIX "")

# Vulkan Memory Allocator (VMA) is vendored as a single header. It is added as a
# SYSTEM include so the compiler does not emit warnings from that third-party
# header (e.g. -Wnullability-completeness), while still flagging issues in our
# own sources.
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        "./putorana")
target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vma
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk
        ${ARCORE_INCLUDE_DIR})

# Pin VMA to Vulkan 1.3 and resolve Vulkan entry points dynamically through
# vkGetInstanceProcAddr/vkGetDeviceProcAddr. Dynamic loading is the robust
# choice on Android, where the NDK's libvulkan.so does not export every core
# 1.3 symbol for direct (static) linking.
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        VMA_VULKAN_VERSION=1003000
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
        # All Vulkan entry points are loaded dynamically through volk. We must
        # NOT also link libvulkan directly: volk defines global vk* symbols (as
        # function pointers), which would collide with libvulkan's exported
        # functions inside this .so. So VK_NO_PROTOTYPES is global and every TU
        # that calls Vulkan goes through volk.
        VK_NO_PROTOTYPES
        # Android WSI surface (vkCreateAndroidSurfaceKHR).
        VK_USE_PLATFORM_ANDROID_KHR
        # Ask for VK_LAYER_KHRONOS_validation in Debug builds only. The layer is
        # still optional at runtime: it is not part of Android, it has to be
        # shipped in app/src/debug/jniLibs/arm64-v8a/, and Instance::Create
        # silently runs without it when the loader cannot find it.
        $<$<CONFIG:Debug>:APP_ENABLE_VALIDATION>)
# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
# OpenChisel: the TSDF reconstruction library behind putorana::recon. Vendored
# and modified rather than consumed as-is: upstream is a catkin package, and the
# local changes alter type layouts, which cannot be done from outside a library.
# See third_party/open_chisel/README.md for provenance and the full diff.
#
# Own CMakeLists rather than a source glob here, because it also has to fetch
# Eigen, and that is nobody else's business in this build.
add_subdirectory(third_party/open_chisel)

# putorana::recon. Its own library, not sources above, because its translation
# units are the only ones allowed to include <open_chisel/...> and they must all
# be built with the NDEBUG and -O3 that Eigen needs. See
# putorana/recon/CMakeLists.txt.
add_subdirectory(putorana/recon)

# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        android
        arcore::sdk_c
        assimp::assimp
        # Not open_chisel directly: it is reached only through putorana_recon,
        # which is what keeps Eigen and its build flags on the far side of that
        # boundary. Linking it here as well would let any TU include its headers.
        putorana_recon
        ${jnigraphics-lib}
        ${camera2ndk-lib}
        ${mediandk-lib}
        log)
