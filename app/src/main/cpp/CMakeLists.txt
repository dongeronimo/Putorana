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
        putorana/graphics/Instance.cpp
        # volk's implementation unit: dlopen's libvulkan.so and defines the
        # global vk* function pointers that every other TU here resolves against.
        third_party/volk/volk.c
        vma_impl.cpp
        native_self_test.cpp
        vulkan_check.cpp
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        native-lib.cpp)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk)

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
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        android
        log)