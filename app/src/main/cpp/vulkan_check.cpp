#include "vulkan_check.h"

#include "volk.h"
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

SelfTestResult RunVulkanVmaSelfTest() {
    // 0. Load the global Vulkan entry points (vkCreateInstance, ...) via volk.
    if (volkInitialize() != VK_SUCCESS) {
        return Fail("volk: failed to initialize (libvulkan not found?)");
    }

    // 1. Create an instance requesting Vulkan 1.3.
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ARReconstructor";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "ARReconstructor";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        return Fail("Vulkan: failed to create a 1.3 instance");
    }
    // Load instance/device-level entry points now that we have an instance.
    volkLoadInstance(instance);

    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);

    // 2. Pick the first physical device and verify it supports Vulkan 1.3.
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        return Fail("Vulkan: no physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    VkPhysicalDevice physicalDevice = devices[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    if (props.apiVersion < VK_API_VERSION_1_3) {
        vkDestroyInstance(instance, nullptr);
        std::ostringstream out;
        out << "Vulkan: device '" << props.deviceName
            << "' only supports " << FormatVersion(props.apiVersion);
        return Fail(out.str());
    }

    // 3. Create a logical device with a single queue from the first family.
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0) {
        vkDestroyInstance(instance, nullptr);
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
        vkDestroyInstance(instance, nullptr);
        return Fail("Vulkan: failed to create logical device");
    }

    // 4. Create a VMA allocator targeting Vulkan 1.3. Entry points are resolved
    // dynamically through the two getter functions (VMA_DYNAMIC_VULKAN_FUNCTIONS).
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
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return Fail("VMA: failed to create allocator");
    }

    // 5. Allocate (and free) a small buffer through VMA to prove it works.
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
            << "Loader API: " << FormatVersion(loaderVersion) << "\n"
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

    // 6. Tear everything down in reverse order.
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    return result;
}
