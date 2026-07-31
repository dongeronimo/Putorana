#include "Image.h"

namespace putorana::graphics {

std::unique_ptr<Image> Image::Create(VmaAllocator allocator, VkDevice device, const Desc& desc,
                                     std::string& error) {
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        error = "image '" + desc.name + "': zero extent";
        return nullptr;
    }

    auto image = std::unique_ptr<Image>(new Image());
    image->allocator_ = allocator;
    image->device_ = device;
    image->format_ = desc.format;
    image->extent_ = desc.extent;
    image->aspect_ = desc.aspect;
    image->name_ = desc.name;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = desc.format;
    imageInfo.extent = {desc.extent.width, desc.extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    // OPTIMAL always. LINEAR exists for CPU-writable images and is slow enough
    // on tiled hardware to be a bug rather than a trade-off.
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = desc.usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Every barrier in this renderer specifies its own oldLayout, and the first
    // one on a fresh image uses UNDEFINED to say "throw the contents away".
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // No HOST_ACCESS flag, so VMA is free to pick pure device-local memory. And
    // DEDICATED_MEMORY for attachments: full screen targets are large, tile
    // hardware often wants them on their own, and it stops one of them from
    // pinning a whole suballocation block alive.
    if ((desc.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }

    const VkResult created = vmaCreateImage(allocator, &imageInfo, &allocInfo, &image->handle_,
                                            &image->allocation_, nullptr);
    if (created != VK_SUCCESS) {
        error = "vmaCreateImage failed for '" + desc.name + "': " + std::to_string(created);
        return nullptr;
    }
    vmaSetAllocationName(allocator, image->allocation_, image->name_.c_str());

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image->handle_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = desc.format;
    viewInfo.subresourceRange.aspectMask = desc.aspect;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    const VkResult viewed = vkCreateImageView(device, &viewInfo, nullptr, &image->view_);
    if (viewed != VK_SUCCESS) {
        error = "vkCreateImageView failed for '" + desc.name + "': " + std::to_string(viewed);
        return nullptr;
    }
    return image;
}

Image::~Image() {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
    }
    if (handle_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, handle_, allocation_);
    }
}

VkFormat FirstSupportedDepthFormat(VkPhysicalDevice physicalDevice, const VkFormat* candidates,
                                   uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, candidates[i], &properties);
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return candidates[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

} // namespace putorana::graphics
