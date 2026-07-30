#include "vulkan_check.h"

#include "vk_mem_alloc.h"

#include <cstdint>
#include <sstream>
#include <vector>

namespace {

// Formats a packed Vulkan version (major.minor.patch) into "x.y.z".
std::string FormatVersion(uint32_t version) {
    std::ostringstream out;
    out << VK_API_VERSION_MAJOR(version) << '.'
        << VK_API_VERSION_MINOR(version) << '.'
        << VK_API_VERSION_PATCH(version);
    return out.str();
}

SelfTestResult Fail(const std::string& message) {
    return SelfTestResult{false, message};
}

} // namespace

SelfTestResult RunVulkanVmaSelfTest(VkInstance instance) {
    if (instance == VK_NULL_HANDLE) {
        return Fail("Vulkan: no instance to run the self-test on");
    }

    // 1. Pick the first physical device and verify it supports Vulkan 1.3.
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        return Fail("Vulkan: no physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    VkPhysicalDevice physicalDevice = devices[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    if (props.apiVersion < VK_API_VERSION_1_3) {
        std::ostringstream out;
        out << "Vulkan: device '" << props.deviceName
            << "' only supports " << FormatVersion(props.apiVersion);
        return Fail(out.str());
    }

    // 2. Create a throwaway logical device with a single queue from the first
    // family. No features are enabled and no extensions are requested: this only
    // needs to be valid enough for VMA to allocate against.
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0) {
        return Fail("Vulkan: device exposes no queue families");
    }

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = 0;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        return Fail("Vulkan: failed to create logical device");
    }

    // The instance was loaded with volkLoadInstanceOnly, so volk's global
    // device-level pointers (vkDestroyDevice among them) are still null. Load them
    // into a local table instead of calling volkLoadDevice: that would point the
    // globals at this throwaway device and leave them dangling once it is
    // destroyed a few lines below.
    VolkDeviceTable deviceTable{};
    volkLoadDeviceTable(&deviceTable, device);

    // 3. Create a VMA allocator targeting Vulkan 1.3. Entry points are resolved
    // dynamically through the two getter functions (VMA_DYNAMIC_VULKAN_FUNCTIONS),
    // so VMA does not care about volk's globals either way.
    VmaVulkanFunctions vulkanFunctions{};
    vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.instance = instance;
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;

    VmaAllocator allocator = VK_NULL_HANDLE;
    if (vmaCreateAllocator(&allocatorInfo, &allocator) != VK_SUCCESS) {
        deviceTable.vkDestroyDevice(device, nullptr);
        return Fail("VMA: failed to create allocator");
    }

    // 4. Allocate (and free) a small buffer through VMA to prove it works.
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = 256;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkResult allocResult =
        vmaCreateBuffer(allocator, &bufferInfo, &allocCreateInfo, &buffer, &allocation, nullptr);

    SelfTestResult result;
    if (allocResult == VK_SUCCESS) {
        vmaDestroyBuffer(allocator, buffer, allocation);
        std::ostringstream out;
        out << "Vulkan 1.3 + VMA OK\n"
            << "GPU: " << props.deviceName << "\n"
            << "Device API: " << FormatVersion(props.apiVersion) << "\n"
            << "Loader API: " << FormatVersion(volkGetInstanceVersion()) << "\n"
            << "VMA: " << VK_API_VERSION_MAJOR(VMA_VERSION) << '.'
            << VK_API_VERSION_MINOR(VMA_VERSION) << '.'
            << VK_API_VERSION_PATCH(VMA_VERSION)
            << " (targeting 1.3)";
        result.ok = true;
        result.report = out.str();
    } else {
        result.ok = false;
        result.report = "VMA: allocator created but buffer allocation failed";
    }

    // 5. Tear down what this function created, in reverse order. The instance is
    // owned by instance_holder and outlives us.
    vmaDestroyAllocator(allocator);
    deviceTable.vkDestroyDevice(device, nullptr);

    return result;
}
