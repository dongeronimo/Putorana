#ifndef PUTORANA_GRAPHICS_CAMERAFEED_H
#define PUTORANA_GRAPHICS_CAMERAFEED_H

#include "Buffer.h"
#include "FrameRing.h"
#include "Image.h"
#include "World.h"
#include "putorana/ar/Subsystem.h"
#include "volk.h"

#include <array>
#include <memory>
#include <string>

namespace putorana::graphics {

class Device;

/**
 * The ARCore camera image, as two Vulkan textures.
 *
 * Deliberately NOT a pass. It opens no rendering scope and draws nothing — it
 * only turns the bytes putorana::ar::Subsystem hands out into something
 * samplable, and the final pass composites it. An earlier version of this did
 * own a pass, drawing the feed into the mesh pass's colour target before the
 * geometry; the attachment then had to be stored and loaded back between the two
 * scopes, which on a tiler costs a full screen read plus a full screen write —
 * around 16MB a frame at 1080p. Compositing in the pass that was already going
 * to read that attachment costs nothing extra.
 *
 * ## Why an upload and not a zero-copy import
 *
 * ARCore can hand out an AHardwareBuffer that Vulkan imports directly, with no
 * copy at all. It is the faster path and it is not the one taken here, because
 * of what it drags in: the buffer has an *external format* known only at
 * runtime, which forces a VkSamplerYcbcrConversion, which must be an immutable
 * sampler baked into a descriptor set layout, which means the pipeline sampling
 * it cannot exist until the first camera frame has arrived. On top of that it is
 * opaque to a graphics debugger, and being able to look at these two textures in
 * RenderDoc is worth more right now than the copy costs.
 *
 * The copy is smaller than it sounds. ARCore's CPU image is not the full sensor
 * — commonly 640x480, so about 460KB at 1.5 bytes per pixel — and no colour
 * conversion happens on the CPU at all: the two planes go up as they are and the
 * fragment shader does the matrix.
 *
 * Changing this decision later touches this file and nothing else. That is the
 * entire reason the subsystem does not speak Vulkan.
 *
 * ## Two textures
 *
 * Luma at full resolution, single channel; chroma at half resolution, two
 * channels interleaved. Sampling both with the same UVs gets 4:2:0's chroma
 * upsampling for free out of the hardware's bilinear filter.
 * */
class CameraFeed {
public:
    static std::unique_ptr<CameraFeed> Create(Device& device, std::string& error);

    ~CameraFeed();

    CameraFeed(const CameraFeed&) = delete;
    CameraFeed& operator=(const CameraFeed&) = delete;

    /**
     * Copies this frame's camera image into the two textures and leaves them in
     * SHADER_READ_ONLY_OPTIMAL.
     *
     * MUST be called outside any rendering scope — it records buffer-to-image
     * copies and image barriers, neither of which is legal between
     * vkCmdBeginRendering and vkCmdEndRendering.
     *
     * Null is the ordinary case for the first frames after a resume, and for any
     * frame the camera did not produce an image for. The textures then keep what
     * they last held, because a repeated frame is far less noticeable than a
     * flash of the clear colour.
     * */
    void Upload(const FrameContext& frame, const ar::CameraFrame* cameraFrame);

    /**
     * True once anything has been uploaded, and therefore once there is a real
     * picture to composite. False makes the mesh pass clear opaquely instead of
     * transparently, so the world still shows something.
     * */
    bool ready() const { return hasContent_; }

    /** LINEAR — this is a genuine rescale, and it upsamples the chroma too. */
    VkSampler sampler() const { return sampler_; }

    /** Null until the first upload has built them. */
    VkImageView lumaView() const { return luma_ != nullptr ? luma_->view() : VK_NULL_HANDLE; }
    VkImageView chromaView() const { return chroma_ != nullptr ? chroma_->view() : VK_NULL_HANDLE; }

    /**
     * Where the three corners of a fullscreen triangle land in the camera image,
     * as u0,v0,u1,v1,u2,v2. Straight from ARCore — see ar::CameraFrame::uv.
     * */
    const float* uv() const { return uv_; }

    /** True when the first byte of each chroma pair is V (NV21) rather than U (NV12). */
    bool vFirst() const { return vFirst_; }

private:
    CameraFeed() = default;

    /** (Re)builds the textures for an image of this size. */
    bool EnsureTextures(const ar::CameraImage& image);

    Device* device_ = nullptr;
    VkDevice handle_ = VK_NULL_HANDLE;

    VkSampler sampler_ = VK_NULL_HANDLE;

    std::unique_ptr<Image> luma_;
    std::unique_ptr<Image> chroma_;
    int32_t imageWidth_ = 0;
    int32_t imageHeight_ = 0;

    /**
     * One per frame in flight. The CPU writes these while the GPU may still be
     * reading the previous frame's copy, which is the same reason the mesh pass
     * keeps its uniform buffers per slot.
     * */
    std::array<std::unique_ptr<Buffer>, FrameRing::kFramesInFlight> staging_{};

    float uv_[6] = {};
    bool vFirst_ = false;
    bool hasContent_ = false;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_CAMERAFEED_H
