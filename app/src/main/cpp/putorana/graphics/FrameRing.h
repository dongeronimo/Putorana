#ifndef PUTORANA_GRAPHICS_FRAMERING_H
#define PUTORANA_GRAPHICS_FRAMERING_H

#include "volk.h"

#include <array>
#include <memory>
#include <string>

namespace putorana::graphics {

/**
 * One slot of the ring: everything a single frame in flight records and waits
 * on. Handed out by FrameRing::BeginFrame and valid until the next one.
 * */
struct FrameSlot {
    uint32_t index = 0;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    /**
     * Signalled by vkAcquireNextImageKHR, waited on by the submission. Binary,
     * and not by choice: the WSI refuses timeline semaphores in acquire and
     * present, because the presentation engine sits outside the queue's timeline
     * and there is no value to hand it.
     * */
    VkSemaphore imageAvailable = VK_NULL_HANDLE;

    /** The timeline value this frame's submission signals when it retires. */
    uint64_t signalValue = 0;
};

/**
 * The ring of frames in flight: command buffers, acquire semaphores, and the
 * single timeline semaphore that tracks which of them have retired.
 *
 * Device lifetime, not swapchain lifetime. None of it references the swapchain,
 * so a resize leaves it alone — the only sync object that has to be rebuilt with
 * the swapchain is the per image renderFinished semaphore, which lives in
 * Swapchain for exactly that reason.
 *
 * The timeline replaces what would otherwise be one VkFence per slot. Frame N
 * signals value N+1, so waiting for N+1-kFramesInFlight before reusing a slot is
 * the whole of the CPU/GPU throttle: one object instead of N, no resetting, and
 * the current value is queryable without blocking.
 * */
class FrameRing {
public:
    /**
     * Two is the useful minimum: one frame recording on the CPU while another
     * runs on the GPU. Three trades latency for smoothness and is worth trying
     * once there is real work per frame.
     * */
    static constexpr uint32_t kFramesInFlight = 2;

    static std::unique_ptr<FrameRing> Create(VkDevice device, uint32_t queueFamily,
                                             std::string& error);

    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    /**
     * Blocks until the slot about to be reused has retired on the GPU, then
     * returns it. The command buffer comes back reset and ready to record.
     * */
    FrameSlot BeginFrame();

    /**
     * Advances the ring. Call only after a submission actually went through:
     * advancing without a submit means waiting forever on a timeline value that
     * will never be signalled.
     * */
    void EndFrame() { ++frameIndex_; }

    VkSemaphore timeline() const { return timeline_; }

private:
    FrameRing() = default;

    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers_{};
    std::array<VkSemaphore, kFramesInFlight> imageAvailable_{};
    VkSemaphore timeline_ = VK_NULL_HANDLE;

    /** Monotonic across the life of the device. Never wraps in any real run. */
    uint64_t frameIndex_ = 0;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_FRAMERING_H
