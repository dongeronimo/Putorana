#include "FrameRing.h"

namespace putorana::graphics {

std::unique_ptr<FrameRing> FrameRing::Create(VkDevice device, uint32_t queueFamily,
                                             std::string& error) {
    auto ring = std::unique_ptr<FrameRing>(new FrameRing());
    ring->device_ = device;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // Lets each buffer be reset on its own, which is what vkBeginCommandBuffer
    // does implicitly. Without it the whole pool has to be reset at once, and
    // that cannot happen while another slot is still in flight.
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &ring->commandPool_) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return nullptr;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ring->commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFramesInFlight;
    if (vkAllocateCommandBuffers(device, &allocInfo, ring->commandBuffers_.data()) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return nullptr;
    }

    VkSemaphoreCreateInfo binaryInfo{};
    binaryInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (vkCreateSemaphore(device, &binaryInfo, nullptr, &ring->imageAvailable_[i]) !=
            VK_SUCCESS) {
            error = "vkCreateSemaphore failed for an acquire semaphore";
            return nullptr;
        }
    }

    // The timeline. Starts at 0; frame N signals N+1, so a value of K means
    // frames 0..K-1 have retired.
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;

    VkSemaphoreCreateInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timelineInfo.pNext = &typeInfo;
    if (vkCreateSemaphore(device, &timelineInfo, nullptr, &ring->timeline_) != VK_SUCCESS) {
        error = "vkCreateSemaphore failed for the timeline";
        return nullptr;
    }

    return ring;
}

FrameRing::~FrameRing() {
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_, nullptr);
    }
    for (VkSemaphore semaphore : imageAvailable_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
    }
    // Destroying the pool frees its command buffers, so they need no separate
    // vkFreeCommandBuffers.
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
    }
}

FrameSlot FrameRing::BeginFrame() {
    const uint32_t index = static_cast<uint32_t>(frameIndex_ % kFramesInFlight);

    // This slot was last used by frame (frameIndex_ - kFramesInFlight), which
    // signalled frameIndex_ + 1 - kFramesInFlight. For the first few frames that
    // expression goes negative; clamping to 0 is a wait that is already
    // satisfied, so the arithmetic works without a special case.
    const uint64_t waitValue = frameIndex_ + 1 >= kFramesInFlight
                                       ? frameIndex_ + 1 - kFramesInFlight
                                       : 0;

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &timeline_;
    waitInfo.pValues = &waitValue;
    vkWaitSemaphores(device_, &waitInfo, UINT64_MAX);

    FrameSlot slot;
    slot.index = index;
    slot.commandBuffer = commandBuffers_[index];
    slot.imageAvailable = imageAvailable_[index];
    slot.signalValue = frameIndex_ + 1;
    return slot;
}

} // namespace putorana::graphics
