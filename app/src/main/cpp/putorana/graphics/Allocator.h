#ifndef PUTORANA_GRAPHICS_ALLOCATOR_H
#define PUTORANA_GRAPHICS_ALLOCATOR_H

// volk before VMA, always. VMA pulls in <vulkan/vulkan.h> itself unless Vulkan
// is already declared, and this target builds with VK_NO_PROTOTYPES — getting
// the order wrong is how you end up with prototypes volk never filled in.
#include "volk.h"
#include "vk_mem_alloc.h"

#include <memory>
#include <string>

namespace putorana::graphics {

/**
 * The one VmaAllocator: every VkBuffer and VkImage in the renderer is carved out
 * of it. Owned by Device, which means it dies and is rebuilt on every trip
 * through Home — anything holding a buffer has to go down with the surface too.
 *
 * Why an allocator at all, when vkAllocateMemory exists: Vulkan caps
 * maxMemoryAllocationCount, and on Android that cap is routinely 4096. One
 * allocation per mesh burns through it. VMA takes big blocks and suballocates,
 * so the count stays flat no matter how many buffers exist.
 *
 * unifiedMemory() is the premise the whole upload path rests on, checked rather
 * than assumed — see Buffer.h.
 * */
class Allocator {
public:
    /**
     * Must be called after volkLoadDevice: VMA is configured with
     * VMA_DYNAMIC_VULKAN_FUNCTIONS, so it resolves what it needs through the
     * two proc-address functions volk has just pointed at this device.
     * */
    static std::unique_ptr<Allocator> Create(VkInstance instance, VkPhysicalDevice physicalDevice,
                                             VkDevice device, std::string& error);

    ~Allocator();

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    VmaAllocator handle() const { return handle_; }

    /**
     * True when the adapter exposes a memory type that is DEVICE_LOCAL and
     * HOST_VISIBLE at once — that is, when the CPU can write straight into
     * memory the GPU reads at full speed and no staging buffer is needed.
     *
     * Every Android GPU is like this, because there is one pool of RAM and the
     * "device local" flag describes access speed rather than a separate chip. It
     * is queried instead of assumed so that the day it is false shows up as a
     * line in logcat and not as mysteriously slow uploads.
     * */
    bool unifiedMemory() const { return unifiedMemory_; }

    /** One line summary, for logcat. */
    std::string Describe() const;

private:
    Allocator() = default;

    VmaAllocator handle_ = VK_NULL_HANDLE;
    bool unifiedMemory_ = false;
    VkDeviceSize deviceLocalBytes_ = 0;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_ALLOCATOR_H
