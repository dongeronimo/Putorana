#include "CameraFeed.h"

#include "Device.h"
#include "Swapchain.h"

#include <android/log.h>

#include <cstring>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

void SetImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                     VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

/**
 * Copies `rows` rows of `bytesPerRow` from a strided source into a tightly
 * packed destination, and answers how many bytes it wrote.
 *
 * The stride is the whole point. A camera plane's rows are padded to whatever
 * suited the producer, so `width * height` is not the size of the data and a
 * single memcpy of that length lands progressively further into the wrong row.
 * When the stride happens to equal the row length this collapses into one
 * memcpy, which is the common case and worth not giving up.
 * */
size_t CopyPlane(uint8_t* destination, const uint8_t* source, int32_t sourceRowStride,
                 int32_t bytesPerRow, int32_t rows) {
    if (sourceRowStride == bytesPerRow) {
        const size_t total = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rows);
        std::memcpy(destination, source, total);
        return total;
    }
    for (int32_t row = 0; row < rows; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * static_cast<size_t>(bytesPerRow),
                    source + static_cast<size_t>(row) * static_cast<size_t>(sourceRowStride),
                    static_cast<size_t>(bytesPerRow));
    }
    return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rows);
}

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

/**
 * A point in the SWAPCHAIN's normalised space, expressed in the VIEW's.
 *
 * The two are not the same space, and the difference is the swapchain's
 * preTransform. The swapchain is created with the surface's currentTransform —
 * see Swapchain.h — which is a promise that the app hands over content already
 * rotated, so that the display controller can present it without a full screen
 * blit. Everything the app draws therefore lives in a space that is rotated
 * relative to what the user is looking at.
 *
 * The mesh pass keeps that promise in Camera::ProjectionMatrix, which multiplies
 * the projection by the matching clip-space rotation. A fullscreen triangle has
 * no projection matrix to hide it in, so it is paid here instead — and it has to
 * be paid, because the texture coordinates it is being fed came from ARCore, and
 * ARCore answers in the VIEW's space.
 *
 * In portrait the preTransform is identity and this is the identity function,
 * which is exactly why a bug here shows up only after the first rotation.
 * */
Vec2 SwapchainToView(VkSurfaceTransformFlagBitsKHR transform, Vec2 p) {
    switch (transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            return {p.y, 1.0f - p.x};
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            return {1.0f - p.x, 1.0f - p.y};
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            return {1.0f - p.y, p.x};
        default:
            // Identity, and the mirrored transforms, which no Android compositor
            // reports — the same judgement Camera::ProjectionMatrix makes.
            return p;
    }
}

/** The three corners fullscreen.vert builds from gl_VertexIndex, in swapchain space. */
constexpr Vec2 kTriangleCorners[3] = {{0.0f, 0.0f}, {2.0f, 0.0f}, {0.0f, 2.0f}};

} // namespace

std::unique_ptr<CameraFeed> CameraFeed::Create(Device& device, std::string& error) {
    auto feed = std::unique_ptr<CameraFeed>(new CameraFeed());
    feed->device_ = &device;
    feed->handle_ = device.handle();

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // LINEAR, unlike the final pass's own NEAREST sampler for the mesh target.
    // That one is a 1:1 copy; this is a genuine rescale — a 640x480 camera image
    // stretched over a 1080p screen — and for the chroma plane the filter is
    // also what does the 4:2:0 upsampling.
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    // CLAMP_TO_EDGE matters here in a way it does not for a 1:1 blit: the UVs
    // come from ARCore's transform and can sit a hair outside [0,1] at the
    // edges, and REPEAT would wrap that into a stripe of the opposite edge.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(feed->handle_, &samplerInfo, nullptr, &feed->sampler_) != VK_SUCCESS) {
        error = "camera feed: could not create the sampler";
        return nullptr;
    }
    return feed;
}

CameraFeed::~CameraFeed() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(handle_, sampler_, nullptr);
    }
}

bool CameraFeed::EnsureTextures(const ar::CameraImage& image) {
    if (luma_ != nullptr && image.width == imageWidth_ && image.height == imageHeight_) {
        return true;
    }
    if (image.width <= 0 || image.height <= 0) {
        return false;
    }
    // Only on the first frame, and on the camera config changing under us. The
    // views about to be replaced may be named by a descriptor a frame in flight
    // is still reading.
    if (luma_ != nullptr) {
        vkDeviceWaitIdle(handle_);
    }

    const VkExtent2D lumaExtent{static_cast<uint32_t>(image.width),
                                static_cast<uint32_t>(image.height)};
    // Rounded up, so an odd dimension keeps its last half-populated chroma
    // column rather than losing it and shifting everything by half a texel.
    const VkExtent2D chromaExtent{static_cast<uint32_t>((image.width + 1) / 2),
                                  static_cast<uint32_t>((image.height + 1) / 2)};

    std::string error;
    Image::Desc lumaDesc;
    lumaDesc.name = "camera luma";
    lumaDesc.extent = lumaExtent;
    lumaDesc.format = VK_FORMAT_R8_UNORM;
    lumaDesc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto luma = Image::Create(device_->allocator().handle(), handle_, lumaDesc, error);
    if (luma == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "camera feed: %s", error.c_str());
        return false;
    }

    Image::Desc chromaDesc;
    chromaDesc.name = "camera chroma";
    chromaDesc.extent = chromaExtent;
    chromaDesc.format = VK_FORMAT_R8G8_UNORM;
    chromaDesc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto chroma = Image::Create(device_->allocator().handle(), handle_, chromaDesc, error);
    if (chroma == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "camera feed: %s", error.c_str());
        return false;
    }

    // 1.5 bytes per pixel: one luma byte, plus one chroma pair per 2x2 block.
    const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(lumaExtent.width) *
                                             static_cast<VkDeviceSize>(lumaExtent.height) +
                                     static_cast<VkDeviceSize>(chromaExtent.width) *
                                             static_cast<VkDeviceSize>(chromaExtent.height) * 2;
    for (uint32_t i = 0; i < FrameRing::kFramesInFlight; ++i) {
        staging_[i] = Buffer::Create(device_->allocator().handle(), stagingSize,
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     "camera staging " + std::to_string(i), error);
        if (staging_[i] == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "camera feed: %s", error.c_str());
            return false;
        }
    }

    luma_ = std::move(luma);
    chroma_ = std::move(chroma);
    imageWidth_ = image.width;
    imageHeight_ = image.height;

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "camera feed: %dx%d, chroma %ux%u", image.width,
                        image.height, chromaExtent.width, chromaExtent.height);
    return true;
}

void CameraFeed::Upload(const FrameContext& frame, const ar::CameraFrame* cameraFrame) {
    if (cameraFrame == nullptr) {
        return;
    }
    const ar::CameraImage& image = cameraFrame->image;
    if (image.y == nullptr || image.uv == nullptr) {
        return;
    }
    if (!EnsureTextures(image)) {
        return;
    }

    // Compose the two mappings the texture coordinates have to survive:
    // swapchain space -> view space (the preTransform), then view space -> image
    // space (ARCore's affine basis). Doing the extrapolation to the triangle's
    // off-screen corners on this side means it is exact by construction, rather
    // than depending on ArFrame_transformCoordinates2d's out-of-domain behaviour.
    const float* basis = cameraFrame->viewToImage;
    const Vec2 origin{basis[0], basis[1]};
    const Vec2 uAxis{basis[2] - origin.x, basis[3] - origin.y};
    const Vec2 vAxis{basis[4] - origin.x, basis[5] - origin.y};

    const VkSurfaceTransformFlagBitsKHR preTransform = frame.swapchain->preTransform();
    for (int i = 0; i < 3; ++i) {
        const Vec2 view = SwapchainToView(preTransform, kTriangleCorners[i]);
        uv_[i * 2] = origin.x + view.x * uAxis.x + view.y * vAxis.x;
        uv_[i * 2 + 1] = origin.y + view.x * uAxis.y + view.y * vAxis.y;
    }
    vFirst_ = image.vFirst;

    const uint32_t chromaWidth = (static_cast<uint32_t>(image.width) + 1) / 2;
    const uint32_t chromaHeight = (static_cast<uint32_t>(image.height) + 1) / 2;

    Buffer& staging = *staging_[frame.frameIndex];
    auto* destination = static_cast<uint8_t*>(staging.MappedAt(0));
    if (destination == nullptr) {
        return;
    }
    const size_t lumaBytes =
            CopyPlane(destination, image.y, image.yRowStride, image.width, image.height);
    const size_t chromaBytes =
            CopyPlane(destination + lumaBytes, image.uv, image.uvRowStride,
                      static_cast<int32_t>(chromaWidth) * 2, static_cast<int32_t>(chromaHeight));
    staging.Flush(0, lumaBytes + chromaBytes);

    // UNDEFINED discards the previous frame's contents, which is right because
    // every texel is about to be overwritten. The source scope still has to name
    // FRAGMENT_SHADER: the previous frame sampled these same two images in the
    // final pass and may still be running, and a barrier's first scope covers
    // everything earlier in submission order. Getting this wrong is a tear that
    // only shows up under load, which is why one pair of textures is enough
    // rather than one per frame in flight.
    SetImageBarrier(frame.commandBuffer, luma_->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
    SetImageBarrier(frame.commandBuffer, chroma_->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy regions[2]{};
    regions[0].bufferOffset = 0;
    regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    regions[0].imageSubresource.layerCount = 1;
    regions[0].imageExtent = {static_cast<uint32_t>(image.width),
                              static_cast<uint32_t>(image.height), 1};
    vkCmdCopyBufferToImage(frame.commandBuffer, staging.handle(), luma_->handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[0]);

    regions[1].bufferOffset = lumaBytes;
    regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    regions[1].imageSubresource.layerCount = 1;
    regions[1].imageExtent = {chromaWidth, chromaHeight, 1};
    vkCmdCopyBufferToImage(frame.commandBuffer, staging.handle(), chroma_->handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[1]);

    SetImageBarrier(frame.commandBuffer, luma_->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
    SetImageBarrier(frame.commandBuffer, chroma_->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);

    hasContent_ = true;
}

} // namespace putorana::graphics
