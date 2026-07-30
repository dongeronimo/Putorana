#ifndef PUTORANA_GRAPHICS_PHYSICALDEVICE_H
#define PUTORANA_GRAPHICS_PHYSICALDEVICE_H

#include "volk.h"

#include <optional>
#include <string>
#include <vector>

namespace putorana::graphics {

/**
 * The features this renderer runs on, as one ready to use pNext chain.
 *
 * Being core in 1.3 buys two things and only two: the entry points exist, and
 * there is no extension string to ask for. It does NOT mean the feature is on —
 * every one of these is off until explicitly turned on right here. Available and
 * enabled are separate questions and both have to be answered.
 *
 * The list is restricted to what a conforming 1.3 implementation is required to
 * support, so it costs no hardware. See the tables in PhysicalDevice.cpp for
 * what that rules out and why.
 *
 * Non-copyable on purpose: the members point at each other through pNext, and a
 * copy would carry pointers into the object it was copied from.
 * */
struct RequiredFeatures {
    RequiredFeatures();
    RequiredFeatures(const RequiredFeatures&) = delete;
    RequiredFeatures& operator=(const RequiredFeatures&) = delete;

    /** Goes straight into VkDeviceCreateInfo::pNext. */
    const void* chain() const { return &features2; }

    VkPhysicalDeviceFeatures2 features2{};
    VkPhysicalDeviceVulkan12Features vulkan12{};
    VkPhysicalDeviceVulkan13Features vulkan13{};
};

/**
 * Device extensions with no core equivalent in 1.3. Short list on purpose:
 * almost everything this renderer wants was promoted into core, and asking for
 * a promoted extension by name is at best redundant and at worst a rejection.
 * */
std::vector<const char*> RequiredDeviceExtensions();

/**
 * Names of the required features the adapter does not support. Empty means it
 * can run the renderer, which on correct 1.3 hardware is always. Reads the same
 * tables RequiredFeatures writes, so the check and the request can never drift
 * apart — and it is what turns a driver that lies about its version into a
 * message that names the feature instead of an opaque vkCreateDevice failure.
 * */
std::vector<std::string> MissingRequiredFeatures(VkPhysicalDevice handle);

/**
 * The chosen adapter, plus what was learned while choosing it.
 *
 * A value type behind std::optional rather than a unique_ptr like Instance,
 * because there is nothing to destroy: a VkPhysicalDevice is a handle the
 * instance already owns, not a resource. Selecting one only reads.
 * */
class PhysicalDevice {
public:
    /**
     * Picks an adapter that can run this renderer and present to the given
     * surface. Returns nullopt and a report naming every rejected candidate and
     * why, because "no suitable GPU" on its own is useless in a bug report.
     * */
    static std::optional<PhysicalDevice> Select(VkInstance instance, VkSurfaceKHR surface,
                                                std::string& error);

    VkPhysicalDevice handle() const { return handle_; }

    /**
     * The one queue family used for everything: graphics, compute, transfer and
     * present. See the note in PhysicalDevice.cpp on why a split is rejected
     * rather than supported.
     * */
    uint32_t queueFamily() const { return queueFamily_; }

    const VkPhysicalDeviceProperties& properties() const { return properties_; }

    /**
     * Mostly here for driverName/driverInfo, which identify the driver far more
     * usefully than the packed driverVersion in properties(). Also carries the
     * update-after-bind limits, should descriptor indexing ever be picked back
     * up.
     * */
    const VkPhysicalDeviceVulkan12Properties& vulkan12Properties() const {
        return vulkan12Properties_;
    }

    /** Multi line summary, for logcat. */
    std::string Describe() const;

private:
    PhysicalDevice() = default;

    VkPhysicalDevice handle_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceVulkan12Properties vulkan12Properties_{};
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_PHYSICALDEVICE_H
