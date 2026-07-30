#ifndef PUTORANA_GRAPHICS_INSTANCE_H
#define PUTORANA_GRAPHICS_INSTANCE_H

#include "volk.h"

#include <memory>
#include <string>
#include <vector>

namespace putorana::graphics {

/**
 * Knobs for Instance::Create. The defaults describe what the renderer needs;
 * everything debug related is opt-in so release builds never pay for it.
 * */
struct InstanceConfig {
    /** Shows up in capture tools (RenderDoc, AGI) and in driver bug reports. */
    const char* applicationName = "ARReconstructor";
    uint32_t applicationVersion = VK_MAKE_VERSION(1, 0, 0);

    /**
     * Highest Vulkan version this code is written against. Create() clamps the
     * request down to what the loader can actually provide and then refuses to
     * continue if the result is lower: there is no fallback path below 1.3,
     * which is also what AndroidManifest.xml declares as a hard uses-feature.
     * */
    uint32_t apiVersion = VK_API_VERSION_1_3;

    /**
     * Enables VK_LAYER_KHRONOS_validation plus a VK_EXT_debug_utils messenger
     * that forwards messages to logcat under the same tag the rest of the native
     * code uses. Best effort: a missing layer or extension is logged and
     * skipped, never fatal.
     *
     * The layer is NOT part of Android: the platform ships only the loader and
     * the vendor driver, so the layer has to travel inside the APK. Grab
     * android-binaries-<version>.zip from the Vulkan-ValidationLayers releases
     * and drop arm64-v8a/libVkLayer_khronos_validation.so into
     *   app/src/debug/jniLibs/arm64-v8a/
     * The Android loader enumerates any libVkLayer_*.so it finds in the app's
     * own native library directory. src/debug rather than src/main keeps the
     * ~15 MB out of the release APK, which could not use it anyway (see the
     * APP_ENABLE_VALIDATION generator expression in CMakeLists.txt).
     * */
    bool enableValidation = false;

    /**
     * Adds the layer's synchronization checks (missing or wrong barriers). Costs
     * CPU time but catches the class of bug that otherwise only reproduces on
     * one vendor's driver. Ignored unless enableValidation is set.
     * */
    bool enableSyncValidation = true;
};

/**
 * The instance is created when the lib is loaded and never destroyed.
 * It supports debug layers optionally.
 *
 * Owns the VkInstance and, when validation is on, the debug messenger attached
 * to it. Nothing else: surface, physical device, device and swapchain have
 * shorter, surface driven lifetimes (see README.md > Graphics Architecture) and
 * are owned by their own classes.
 * */
class Instance {
public:
    /**
     * Initializes volk and creates the instance. Returns nullptr on failure and
     * writes a human readable reason into error.
     * */
    static std::unique_ptr<Instance> Create(const InstanceConfig& config, std::string& error);

    ~Instance();

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    /**
     * Call this function to get the vkInstance object
     * */
    VkInstance handle() const { return instance_; }

    /** Version the instance was created with: min(loader, requested). */
    uint32_t apiVersion() const { return apiVersion_; }

    /** What vkEnumerateInstanceVersion reported, before clamping. */
    uint32_t loaderVersion() const { return loaderVersion_; }

    /** True only if the validation layer was actually found and enabled. */
    bool validationEnabled() const { return validationEnabled_; }

    /**
     * Whether a given instance extension ended up enabled. Device level code
     * uses this to decide whether vkSetDebugUtilsObjectNameEXT is safe to call.
     * */
    bool IsExtensionEnabled(const char* name) const;

    /** Multi line summary, for logcat and for the UI. */
    std::string Describe() const;

private:
    Instance() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    uint32_t apiVersion_ = 0;
    uint32_t loaderVersion_ = 0;
    bool validationEnabled_ = false;
    std::vector<std::string> enabledLayers_;
    std::vector<std::string> enabledExtensions_;
};

/**
 * The instance's lifetime is the lifetime of the loaded native library, so it
 * is created from JNI_OnLoad (see native-lib.cpp) instead of being threaded
 * through constructors. Android never unloads the library in practice, so
 * nothing calls DestroyInstance outside of tests.
 * */
namespace instance_holder {

/** Creates the library wide instance and logs the outcome. Called once. */
void CreateInstance(bool enableValidation);

/** Releases it. Safe to call even when creation failed. */
void DestroyInstance();

/** The instance, or nullptr when creation failed. */
Instance* Get();

/** Why Get() is null. Empty when it is not. */
const std::string& Error();

} // namespace instance_holder

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_INSTANCE_H
