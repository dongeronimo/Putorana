#include "Allocator.h"

#include <android/log.h>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

} // namespace

std::unique_ptr<Allocator> Allocator::Create(VkInstance instance, VkPhysicalDevice physicalDevice,
                                             VkDevice device, std::string& error) {
    auto allocator = std::unique_ptr<Allocator>(new Allocator());

    // With VMA_STATIC_VULKAN_FUNCTIONS=0 and VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    // (both set in CMakeLists.txt) these two are the only entry points VMA
    // cannot find on its own; it fetches the rest through them. Leaving them
    // null is the classic volk+VMA crash: a table of null pointers, called on
    // the first allocation.
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    // Has to match what the device was actually created with. Claiming a higher
    // version makes VMA call entry points that may not exist; claiming a lower
    // one silently disables the 1.1+ paths, including the dedicated allocation
    // handling that image attachments want.
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance = instance;
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.pVulkanFunctions = &functions;

    const VkResult result = vmaCreateAllocator(&info, &allocator->handle_);
    if (result != VK_SUCCESS) {
        error = "vmaCreateAllocator failed: " + std::to_string(result);
        return nullptr;
    }

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        const VkMemoryPropertyFlags wanted =
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if ((flags & wanted) == wanted) {
            allocator->unifiedMemory_ = true;
            break;
        }
    }
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            allocator->deviceLocalBytes_ += memory.memoryHeaps[i].size;
        }
    }

    if (!allocator->unifiedMemory_) {
        // Not fatal, but every upload path in this renderer writes straight from
        // the CPU into GPU-visible memory. On a device without a unified memory
        // type VMA falls back to plain HOST_VISIBLE and the buffers stay off the
        // fast heap; the fix would be staging buffers, which nothing here has.
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "no DEVICE_LOCAL|HOST_VISIBLE memory type: uploads will not land in "
                            "fast memory and this renderer has no staging path");
    }

    return allocator;
}

Allocator::~Allocator() {
    if (handle_ != VK_NULL_HANDLE) {
        // Asserts in debug builds if any allocation is still alive, which is the
        // leak check we get for free: it fires precisely when something outlived
        // the Device it was allocated from.
        vmaDestroyAllocator(handle_);
    }
}

std::string Allocator::Describe() const {
    return "device local heaps " + std::to_string(deviceLocalBytes_ / (1024 * 1024)) + " MiB, " +
           (unifiedMemory_ ? "unified memory (no staging needed)" : "NO unified memory type");
}

} // namespace putorana::graphics
