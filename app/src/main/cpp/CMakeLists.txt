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

# ARCore. O cabeçalho e a biblioteca vêm de sítios diferentes e comportam-se de
# maneira diferente nesta build — ver app/src/main/cpp/third_party/arcore/README.md.
#
# O cabeçalho está vendorizado, logo existe sempre e é seguro exigi-lo: se
# faltar, é um clone incompleto e mais vale dizê-lo já do que despejar centenas
# de erros de "unknown type ArSession".
set(ARCORE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/arcore/include)
if(NOT EXISTS ${ARCORE_INCLUDE_DIR}/arcore_c_api.h)
    message(FATAL_ERROR
            "arcore_c_api.h não está em ${ARCORE_INCLUDE_DIR}. "
            "Ver third_party/arcore/README.md para o obter.")
endif()

# A biblioteca vem do AAR do Maven. Descarrega-se aqui, no configure, pela mesma
# razão que o assimp vem por FetchContent logo acima: é a altura em que o CMake
# sabe o que precisa e ainda pode falhar de forma legível.
#
# Porquê não deixar isto ao Gradle, que já resolve o mesmo AAR para o
# `implementation`? Porque o AGP empacota o jni/ de um AAR no APK mas nunca
# expõe esse caminho à build nativa — não há nada para onde apontar o
# IMPORTED_LOCATION. Descompactá-lo numa task de Gradle (o que fazem os samples
# C do SDK) obriga a resolver uma configuration durante a construção do grafo de
# tasks, o que o AGP assinala como resolução em configuration time em todas as
# builds, e mesmo assim deixaria o ficheiro em falta durante o sync do Android
# Studio, que corre este configure fora do grafo de tasks.
#
# ARCORE_VERSION vem do version catalog, via app/build.gradle.kts.
if(NOT DEFINED ARCORE_VERSION)
    message(FATAL_ERROR "ARCORE_VERSION não foi definida — devia vir de app/build.gradle.kts.")
endif()

set(ARCORE_AAR ${CMAKE_CURRENT_BINARY_DIR}/arcore/core-${ARCORE_VERSION}.aar)
set(ARCORE_UNPACKED ${CMAKE_CURRENT_BINARY_DIR}/arcore/unpacked)

# Sem URL_HASH de propósito: um hash fixo aqui teria de ser atualizado a cada
# bump de versão, num ficheiro diferente daquele que decide a versão, e a
# proteção que dá é a mesma que o Gradle já dá (ou não dá) ao descarregar
# exatamente este artefacto para o `implementation` — HTTPS para dl.google.com,
# sem dependency verification. Não vale o terceiro sítio a manter em sincronia.
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
        # O ficheiro parcial tem de desaparecer, senão o EXISTS acima dá-o por
        # bom na próxima build e o erro reaparece lá à frente, no link.
        file(REMOVE ${ARCORE_AAR})
        message(FATAL_ERROR "Falhou o download do AAR do ARCore: ${ARCORE_DOWNLOAD_ERROR}")
    endif()
    # ARCHIVE_EXTRACT reconhece o zip pelo conteúdo, não pela extensão .aar.
    file(ARCHIVE_EXTRACT INPUT ${ARCORE_AAR} DESTINATION ${ARCORE_UNPACKED} PATTERNS "jni/*")
endif()

set(ARCORE_LIB ${ARCORE_UNPACKED}/jni/${ANDROID_ABI}/libarcore_sdk_c.so)
if(NOT EXISTS ${ARCORE_LIB})
    message(FATAL_ERROR "O AAR do ARCore não trazia jni/${ANDROID_ABI}/libarcore_sdk_c.so.")
endif()

# IMPORTED em vez de find_library: o .so não está no sysroot do NDK, é um
# artefacto descarregado num caminho absoluto que já conhecemos.
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
        putorana/graphics/Allocator.cpp
        putorana/graphics/Buffer.cpp
        putorana/graphics/ArCamera.cpp
        putorana/graphics/Camera.cpp
        putorana/graphics/CameraFeed.cpp
        putorana/graphics/DescriptorPool.cpp
        putorana/graphics/Device.cpp
        putorana/graphics/FinalPass.cpp
        putorana/graphics/FlatColorMaterial.cpp
        putorana/graphics/Frame.cpp
        putorana/graphics/FrameRing.cpp
        putorana/graphics/GltfLoader.cpp
        putorana/graphics/GpuProfiler.cpp
        putorana/graphics/HelloWorld.cpp
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
# for the name without it — an UnsatisfiedLinkError at the first line of
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
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        android
        arcore::sdk_c
        assimp::assimp
        ${jnigraphics-lib}
        ${camera2ndk-lib}
        ${mediandk-lib}
        log)
