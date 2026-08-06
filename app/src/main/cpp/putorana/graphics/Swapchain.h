#ifndef PUTORANA_GRAPHICS_SWAPCHAIN_H
#define PUTORANA_GRAPHICS_SWAPCHAIN_H

#include "volk.h"

#include <memory>
#include <string>
#include <vector>

namespace putorana::graphics {

/**
 * The images the compositor actually shows, plus the two things whose lifetime
 * is welded to them: one view per image, and one "render finished" semaphore per
 * image.
 *
 * Nested inside the surface's lifetime — a swapchain never outlives the
 * VkSurfaceKHR it was built on — but shorter, because a resize or a rotation
 * replaces it while the surface stays put. Device owns exactly one at a time and
 * swaps it out through RecreateSwapchainIfNeeded.
 *
 * Render thread only, like everything else under Device.
 * */
class Swapchain {
public:
    /**
     * Builds a swapchain sized to the surface's current extent.
     *
     * oldSwapchain may be VK_NULL_HANDLE, or the handle of the swapchain being
     * replaced — passing it lets the driver hand buffers straight over instead
     * of allocating a second full set. The caller still owns the old object and
     * must destroy it, but only *after* this returns.
     * */
    static std::unique_ptr<Swapchain> Create(VkPhysicalDevice physicalDevice, VkDevice device,
                                             VkSurfaceKHR surface, VkSwapchainKHR oldSwapchain,
                                             std::string& error);

    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    VkSwapchainKHR handle() const { return handle_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }

    /**
     * The rotation baked into the swapchain, taken from the surface's
     * currentTransform so the compositor never has to rotate a presented frame
     * itself. On a phone that is the difference between a free present and a
     * full screen blit every frame.
     *
     * The cost is that it is now the renderer's job: whatever projection matrix
     * eventually gets used has to apply this same rotation, and when it is 90 or
     * 270 degrees the width and height it should reason about are swapped
     * relative to extent(). While nothing is drawn but a clear, this is
     * invisible — the first rotated triangle is where it shows up.
     * */
    VkSurfaceTransformFlagBitsKHR preTransform() const { return preTransform_; }

    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    VkImage image(uint32_t index) const { return images_[index]; }
    VkImageView imageView(uint32_t index) const { return imageViews_[index]; }

    /**
     * Signalled by the frame's submission and waited on by vkQueuePresentKHR.
     *
     * Indexed by swapchain image, NOT by frame in flight, and that is not an
     * arbitrary choice. Nothing ever tells the CPU that the presentation engine
     * has finished waiting on this semaphore — the in-flight fence only proves
     * the *submission* retired. Reusing one across frames therefore races. One
     * per image is safe because the same image cannot be acquired again until
     * the presentation engine is done with it, which is exactly the guarantee
     * vkAcquireNextImageKHR provides.
     * */
    VkSemaphore renderFinishedSemaphore(uint32_t imageIndex) const {
        return renderFinished_[imageIndex];
    }

    /** One line summary, for logcat. */
    std::string Describe() const;

private:
    Swapchain() = default;

    VkDevice device_ = VK_NULL_HANDLE;
    VkSwapchainKHR handle_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkSurfaceTransformFlagBitsKHR preTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

    /** Owned by the swapchain: acquired, never created, never destroyed. */
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    std::vector<VkSemaphore> renderFinished_;
};

/**
 * True for the _SRGB colour formats the swapchain is willing to choose.
 *
 * ## Every shader that writes colour has to ask this
 *
 * With an _SRGB attachment the hardware encodes on write and decodes on sample,
 * so a shader hands over and receives LINEAR values. With a _UNORM one nothing
 * happens in either direction and the same shader has to hand over gamma-encoded
 * values instead. Getting it backwards is not subtle at the dark end: a linear
 * value written to _UNORM is the classic washed-out picture, and a gamma value
 * written to _SRGB is the equally classic muddy one.
 *
 * The _SRGB entries are preferred, so this is normally true. It is not a
 * constant, because the fallbacks in ChooseFormat are real and a surface is only
 * required to expose one format of any kind.
 *
 * Lives here rather than in each pass because the format is chosen here, and
 * because two passes deciding this independently is two places to get it wrong:
 * the camera feed and the reconstruction drawn in front of it must agree, or the
 * geometry reads as a different colour from the pixels around it.
 * */
bool IsSrgbFormat(VkFormat format);

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_SWAPCHAIN_H
