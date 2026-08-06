#include "Swapchain.h"

#include <algorithm>
#include <sstream>

namespace putorana::graphics {

namespace {

/**
 * In preference order. The _SRGB entries come first so the hardware does the
 * gamma conversion on write, for free — with a _UNORM surface the shader has to
 * do it, and forgetting is how a renderer ends up looking washed out.
 * */
constexpr VkFormat kPreferredFormats[] = {
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
};

VkSurfaceFormatKHR ChooseFormat(const std::vector<VkSurfaceFormatKHR>& available) {
    for (VkFormat preferred : kPreferredFormats) {
        for (const VkSurfaceFormatKHR& candidate : available) {
            if (candidate.format == preferred &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return candidate;
            }
        }
    }
    // The surface is required to expose at least one format, so this is a
    // working fallback rather than a failure — it just might be 565.
    return available.front();
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    // OPAQUE first: this window is the whole screen and nothing behind it should
    // show through. Android commonly offers INHERIT instead, which defers to
    // whatever the ANativeWindow was configured with.
    constexpr VkCompositeAlphaFlagBitsKHR kOrder[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR candidate : kOrder) {
        if ((supported & candidate) != 0) {
            return candidate;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

const char* TransformName(VkSurfaceTransformFlagBitsKHR transform) {
    switch (transform) {
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
            return "identity";
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            return "rotate 90";
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            return "rotate 180";
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            return "rotate 270";
        default:
            return "mirrored or unknown";
    }
}

const char* FormatName(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
            return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_SRGB:
            return "B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
            return "R5G6B5_UNORM_PACK16";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return "A2B10G10R10_UNORM_PACK32";
        default:
            return "other";
    }
}

} // namespace

bool IsSrgbFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return true;
        default:
            return false;
    }
}

std::unique_ptr<Swapchain> Swapchain::Create(VkPhysicalDevice physicalDevice, VkDevice device,
                                             VkSurfaceKHR surface, VkSwapchainKHR oldSwapchain,
                                             std::string& error) {
    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities) !=
        VK_SUCCESS) {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return nullptr;
    }

    // The spec says the behaviour is platform-dependent when imageExtent does
    // not match currentExtent, so it is taken verbatim. Android always reports a
    // real value here (never the 0xFFFFFFFF "pick anything" sentinel some
    // desktop window systems use), which is why the size the SurfaceHolder
    // reported is only ever used to notice that something changed.
    if (capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0) {
        error = "surface has zero area";
        return nullptr;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (formatCount == 0) {
        error = "surface exposes no formats";
        return nullptr;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    const VkSurfaceFormatKHR surfaceFormat = ChooseFormat(formats);

    // One more than the minimum, so the CPU is never stuck waiting for the
    // compositor to hand an image back. maxImageCount == 0 means unlimited.
    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = capabilities.currentExtent;
    createInfo.imageArrayLayers = 1;
    // The only usage guaranteed to be supported. Anything else has to be checked
    // against capabilities.supportedUsageFlags first.
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // One queue family does graphics and present (PhysicalDevice enforces it),
    // so no image is ever shared and no ownership transfer is needed.
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Pre-rotation. Matching currentTransform means the presentation engine does
    // nothing at present time; anything else makes it rotate every frame.
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
    // FIFO is the only mode every implementation must support, and on a phone it
    // is also the right one: Choreographer already paces the loop to vsync, so
    // MAILBOX would only render frames nobody sees and drain the battery.
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    // Lets the driver skip fragments hidden by another window. Fine because
    // nothing here reads the presented image back.
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = oldSwapchain;

    auto swapchain = std::unique_ptr<Swapchain>(new Swapchain());
    swapchain->device_ = device;
    swapchain->format_ = surfaceFormat.format;
    swapchain->extent_ = capabilities.currentExtent;
    swapchain->preTransform_ = capabilities.currentTransform;

    const VkResult result =
            vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain->handle_);
    if (result != VK_SUCCESS) {
        std::ostringstream out;
        out << "vkCreateSwapchainKHR failed: " << result;
        error = out.str();
        swapchain->handle_ = VK_NULL_HANDLE;
        return nullptr;
    }

    // The driver decides the real count and it can exceed what was asked for.
    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain->handle_, &actualCount, nullptr);
    swapchain->images_.resize(actualCount);
    vkGetSwapchainImagesKHR(device, swapchain->handle_, &actualCount, swapchain->images_.data());

    swapchain->imageViews_.resize(actualCount, VK_NULL_HANDLE);
    swapchain->renderFinished_.resize(actualCount, VK_NULL_HANDLE);

    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain->images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain->format_;
        viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &swapchain->imageViews_[i]) !=
            VK_SUCCESS) {
            error = "vkCreateImageView failed for a swapchain image";
            return nullptr;
        }

        // One per image, not per frame in flight. See the note on
        // renderFinishedSemaphore() for why that distinction is a correctness
        // issue rather than a style choice.
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &swapchain->renderFinished_[i]) !=
            VK_SUCCESS) {
            error = "vkCreateSemaphore failed for a swapchain image";
            return nullptr;
        }
    }

    return swapchain;
}

Swapchain::~Swapchain() {
    // Reverse of creation. The images themselves are not destroyed: they belong
    // to the swapchain and go with it.
    for (VkSemaphore semaphore : renderFinished_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
    }
    for (VkImageView view : imageViews_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    if (handle_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, handle_, nullptr);
    }
}

std::string Swapchain::Describe() const {
    std::ostringstream out;
    out << extent_.width << "x" << extent_.height << ", " << images_.size() << " images, "
        << FormatName(format_) << ", pre-transform " << TransformName(preTransform_);
    return out.str();
}

} // namespace putorana::graphics
