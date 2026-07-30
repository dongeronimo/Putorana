#include "PhysicalDevice.h"

#include <algorithm>
#include <sstream>

namespace putorana::graphics {

namespace {

/**
 * A required feature: the bit to set, and a name to print when the driver says
 * no. Pointer-to-member so the "is it supported" check and the "turn it on"
 * request read the same table — writing those as two separate lists is how a
 * renderer ends up asking for something it never verified.
 * */
template <typename FeatureStruct>
struct FeatureFlag {
    const char* name;
    VkBool32 FeatureStruct::*member;
};

/**
 * The rule for both tables below: nothing goes in that a conforming Vulkan 1.3
 * device is allowed to refuse.
 *
 * That is what keeps this renderer at one code path. A feature that can
 * legitimately be absent forces a choice between branching at every use site or
 * refusing to start on hardware that is otherwise fine, and neither is wanted
 * here. So the check exists to name a broken driver, not to filter hardware — on
 * correct hardware it can never fail.
 *
 * The casualty of that rule is bindless. VK_EXT_descriptor_indexing was promoted
 * into 1.2 core, but every one of its feature bits stayed optional, so
 * runtimeDescriptorArray and friends are out. Same for bufferDeviceAddress.
 * */
constexpr FeatureFlag<VkPhysicalDeviceVulkan12Features> kRequired12[] = {
        // On Vulkan 1.2's mandatory list. Nothing uses it yet, but every
        // submission model worth writing does, and adding it later would mean
        // touching device creation again.
        {"timelineSemaphore", &VkPhysicalDeviceVulkan12Features::timelineSemaphore},
};

/**
 * Mandatory for any Vulkan 1.3 implementation, per the spec's Feature
 * Requirements section.
 *
 * Free to add from here whenever something needs them, all equally guaranteed
 * and all currently unused: robustImageAccess, inlineUniformBlock,
 * pipelineCreationCacheControl, privateData, shaderDemoteToHelperInvocation,
 * shaderTerminateInvocation, subgroupSizeControl, computeFullSubgroups,
 * shaderZeroInitializeWorkgroupMemory, shaderIntegerDotProduct.
 *
 * The two members of VkPhysicalDeviceVulkan13Features that are NOT guaranteed,
 * despite sitting in the same struct: textureCompressionASTC_HDR and
 * descriptorBindingInlineUniformBlockUpdateAfterBind. Sharing a struct with the
 * core version says nothing about being required by it.
 * */
constexpr FeatureFlag<VkPhysicalDeviceVulkan13Features> kRequired13[] = {
        // No VkRenderPass, no VkFramebuffer.
        {"dynamicRendering", &VkPhysicalDeviceVulkan13Features::dynamicRendering},
        // vkCmdPipelineBarrier2 and friends, with stage/access masks that can
        // express what the old ones could not.
        {"synchronization2", &VkPhysicalDeviceVulkan13Features::synchronization2},
        // vkGetDeviceBufferMemoryRequirements without building the buffer first,
        // plus relaxed interface matching between shader stages.
        {"maintenance4", &VkPhysicalDeviceVulkan13Features::maintenance4},
};

/** Fills a chain with what the adapter supports, so it can be diffed. */
void QuerySupportedFeatures(VkPhysicalDevice handle, VkPhysicalDeviceFeatures2& features2,
                            VkPhysicalDeviceVulkan12Features& vulkan12,
                            VkPhysicalDeviceVulkan13Features& vulkan13) {
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.pNext = &vulkan13;
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vulkan12;
    vkGetPhysicalDeviceFeatures2(handle, &features2);
}

std::vector<std::string> MissingDeviceExtensions(VkPhysicalDevice handle) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(handle, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    if (count > 0) {
        vkEnumerateDeviceExtensionProperties(handle, nullptr, &count, available.data());
    }

    std::vector<std::string> missing;
    for (const char* required : RequiredDeviceExtensions()) {
        const bool found = std::any_of(available.begin(), available.end(),
                                       [required](const VkExtensionProperties& properties) {
                                           return std::string(properties.extensionName) == required;
                                       });
        if (!found) {
            missing.emplace_back(required);
        }
    }
    return missing;
}

/**
 * Finds one family that does graphics and can present.
 *
 * Deliberately does not support a graphics family and a present family being
 * different. That case forces either VK_SHARING_MODE_CONCURRENT on every
 * swapchain image or an explicit ownership transfer around each present, and it
 * does not exist on Android: Adreno, Mali and PowerVR all expose present on the
 * graphics family. Failing loudly beats carrying a code path that is never taken
 * and therefore never right.
 * */
std::optional<uint32_t> FindGraphicsPresentFamily(VkPhysicalDevice handle, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(handle, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    if (count == 0) {
        return std::nullopt;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(handle, &count, families.data());

    for (uint32_t index = 0; index < count; ++index) {
        if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        // The only way to ask this on Android. VK_KHR_android_surface has no
        // vkGetPhysicalDevice*PresentationSupportKHR, so the surface has to
        // exist first — which is exactly why device creation lives in
        // OnSurfaceCreated instead of next to the instance.
        VkBool32 canPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(handle, index, surface, &canPresent);
        if (canPresent == VK_TRUE) {
            return index;
        }
    }
    return std::nullopt;
}

int ScoreDevice(const VkPhysicalDeviceProperties& properties) {
    switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 1;
        default:
            // CPU and OTHER. A software rasterizer would technically pass every
            // check above and then run at one frame per second.
            return 0;
    }
}

std::string FormatVersion(uint32_t version) {
    std::ostringstream out;
    out << VK_API_VERSION_MAJOR(version) << '.' << VK_API_VERSION_MINOR(version) << '.'
        << VK_API_VERSION_PATCH(version);
    return out.str();
}

std::string Join(const std::vector<std::string>& items) {
    std::ostringstream out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << items[i];
    }
    return out.str();
}

} // namespace

RequiredFeatures::RequiredFeatures() {
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.pNext = &vulkan13;
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vulkan12;

    for (const auto& flag : kRequired12) {
        vulkan12.*(flag.member) = VK_TRUE;
    }
    for (const auto& flag : kRequired13) {
        vulkan13.*(flag.member) = VK_TRUE;
    }

    // features2.features (the plain Vulkan 1.0 set) is left entirely zeroed:
    // nothing here needs samplerAnisotropy or the like yet, and enabling an
    // optional 1.0 feature without checking it first is a vkCreateDevice
    // failure waiting to happen. Anything added there belongs in a third table
    // next to kRequired12/kRequired13, not turned on by hand.
}

std::vector<const char*> RequiredDeviceExtensions() {
    // VK_KHR_swapchain is not core in any Vulkan version and never will be —
    // presenting is a window system concern, not a rendering one. Everything
    // else this renderer uses came in through 1.2/1.3 core.
    return {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
}

std::vector<std::string> MissingRequiredFeatures(VkPhysicalDevice handle) {
    VkPhysicalDeviceFeatures2 features2{};
    VkPhysicalDeviceVulkan12Features vulkan12{};
    VkPhysicalDeviceVulkan13Features vulkan13{};
    QuerySupportedFeatures(handle, features2, vulkan12, vulkan13);

    std::vector<std::string> missing;
    for (const auto& flag : kRequired12) {
        if (vulkan12.*(flag.member) != VK_TRUE) {
            missing.emplace_back(flag.name);
        }
    }
    for (const auto& flag : kRequired13) {
        if (vulkan13.*(flag.member) != VK_TRUE) {
            missing.emplace_back(flag.name);
        }
    }
    return missing;
}

std::optional<PhysicalDevice> PhysicalDevice::Select(VkInstance instance, VkSurfaceKHR surface,
                                                     std::string& error) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        error = "no Vulkan physical devices found";
        return std::nullopt;
    }
    std::vector<VkPhysicalDevice> handles(count);
    vkEnumeratePhysicalDevices(instance, &count, handles.data());

    std::optional<PhysicalDevice> best;
    int bestScore = -1;
    std::ostringstream rejections;

    for (VkPhysicalDevice handle : handles) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(handle, &properties);

        // The instance being 1.3 says nothing about the device. The loader
        // clamps its own version, but each adapter reports its own, and an
        // implementation at 1.1 or above is forbidden from failing
        // vkCreateInstance over a version it does not support — so this is the
        // first place the truth shows up.
        if (properties.apiVersion < VK_API_VERSION_1_3) {
            rejections << "\n  " << properties.deviceName << ": only Vulkan "
                       << FormatVersion(properties.apiVersion);
            continue;
        }

        const std::vector<std::string> missingExtensions = MissingDeviceExtensions(handle);
        if (!missingExtensions.empty()) {
            rejections << "\n  " << properties.deviceName
                       << ": missing extensions: " << Join(missingExtensions);
            continue;
        }

        const std::vector<std::string> missingFeatures = MissingRequiredFeatures(handle);
        if (!missingFeatures.empty()) {
            rejections << "\n  " << properties.deviceName
                       << ": missing features: " << Join(missingFeatures);
            continue;
        }

        const std::optional<uint32_t> family = FindGraphicsPresentFamily(handle, surface);
        if (!family.has_value()) {
            rejections << "\n  " << properties.deviceName
                       << ": no queue family does graphics and present";
            continue;
        }

        const int score = ScoreDevice(properties);
        if (score <= bestScore) {
            continue;
        }

        PhysicalDevice candidate;
        candidate.handle_ = handle;
        candidate.queueFamily_ = family.value();
        candidate.properties_ = properties;

        candidate.vulkan12Properties_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &candidate.vulkan12Properties_;
        vkGetPhysicalDeviceProperties2(handle, &properties2);

        best = std::move(candidate);
        bestScore = score;
    }

    if (!best.has_value()) {
        error = "no GPU can run this renderer:" + rejections.str();
    }
    return best;
}

std::string PhysicalDevice::Describe() const {
    std::ostringstream out;
    out << "GPU: " << properties_.deviceName << "\n"
        << "Device API: " << FormatVersion(properties_.apiVersion) << "\n"
        // driverName/driverInfo are the vendor's own strings and say far more in
        // a bug report than the packed driverVersion integer does.
        << "Driver: " << vulkan12Properties_.driverName << " " << vulkan12Properties_.driverInfo
        << "\n"
        << "Queue family: " << queueFamily_ << " (graphics + present)";
    return out.str();
}

} // namespace putorana::graphics
