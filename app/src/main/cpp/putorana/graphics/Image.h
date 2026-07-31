#ifndef PUTORANA_GRAPHICS_IMAGE_H
#define PUTORANA_GRAPHICS_IMAGE_H

#include "volk.h"
#include "vk_mem_alloc.h"

#include <memory>
#include <string>

namespace putorana::graphics {

/**
 * A VkImage from VMA plus the one view onto it, which in practice is always
 * wanted together — the counterpart of Buffer for attachments and textures.
 *
 * Unlike Buffer this is never host-mapped. A render target is written by the
 * GPU, and even a texture goes to OPTIMAL tiling, whose memory layout is the
 * driver's business and not something the CPU can meaningfully write into.
 * Uploading pixels is therefore a staging buffer and a vkCmdCopyBufferToImage
 * — the one place "Android has unified memory so no staging is needed" does
 * NOT apply, because the obstacle there is tiling, not locality.
 * */
class Image {
public:
    struct Desc {
        std::string name;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        /** COLOR_BIT or DEPTH_BIT — used by the view and by every barrier on it. */
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    };

    static std::unique_ptr<Image> Create(VmaAllocator allocator, VkDevice device, const Desc& desc,
                                         std::string& error);

    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    VkImage handle() const { return handle_; }
    VkImageView view() const { return view_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    VkImageAspectFlags aspect() const { return aspect_; }

private:
    Image() = default;

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage handle_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_COLOR_BIT;
    std::string name_;
};

/**
 * The first of `candidates` the adapter can use as a depth attachment.
 *
 * Has to be asked rather than assumed: the spec guarantees only that at least
 * ONE of VK_FORMAT_D32_SFLOAT and VK_FORMAT_X8_D24_UNORM_PACK32 works, never a
 * specific one. Returns VK_FORMAT_UNDEFINED when none of them do.
 * */
VkFormat FirstSupportedDepthFormat(VkPhysicalDevice physicalDevice,
                                   const VkFormat* candidates, uint32_t count);

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_IMAGE_H
