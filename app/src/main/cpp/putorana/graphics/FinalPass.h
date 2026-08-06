#ifndef PUTORANA_GRAPHICS_FINALPASS_H
#define PUTORANA_GRAPHICS_FINALPASS_H

#include "CameraFeed.h"
#include "Image.h"
#include "World.h"
#include "volk.h"

#include <memory>
#include <string>

namespace putorana::graphics {

class Device;

/**
 * Puts what the mesh pass drew onto the image that gets presented, by drawing
 * one screen-covering triangle that samples it, and, when there is one, puts
 * the camera feed underneath it in the same draw.
 *
 * ## Why this exists at all
 *
 * The mesh pass could draw straight into the swapchain image and save a full
 * screen write plus a full screen read, which on a phone is real bandwidth, and
 * this pass is not free. It is here because everything that has to happen to a
 * finished image happens in it: compositing the virtual world onto the real one,
 * tone mapping the day the mesh pass draws into a float target and light is
 * allowed past 1.0, and any post effect after that. Those are not things a
 * geometry pass can do to itself, because they need the whole image and it is
 * still being written.
 *
 * ## Why the camera feed composites HERE
 *
 * Because this pass was already going to read the mesh pass's attachment, so the
 * background costs one extra texture fetch and nothing else. The alternative,
 * a camera pass drawing into that attachment before the mesh pass, forces the
 * attachment to be stored and loaded back between the two scopes: a full screen
 * read plus an extra full screen write, around 16MB a frame at 1080p, for a
 * result that draw order gives away.
 *
 * The compositing is a mix by the mesh target's alpha, not fixed-function
 * blending. Doing it in the shader keeps the colour space explicit, which
 * matters when one input is linear (an _SRGB texture, decoded on sample) and the
 * other is gamma-encoded camera data.
 *
 * ## Its own set 0
 *
 * Nothing here shares the mesh pass's descriptor layouts. There is no camera, no
 * per-object array and no material: three combined image samplers, and it is
 * set 0 because it is the only set there is. A pass gets to define its own
 * numbering; the three-set contract in Material.h is about shaders that draw
 * geometry.
 * */
class FinalPass {
public:
    /** `targetFormat` is the swapchain's, since that is what this draws into. */
    static std::unique_ptr<FinalPass> Create(Device& device, VkFormat targetFormat,
                                             std::string& error);

    ~FinalPass();

    FinalPass(const FinalPass&) = delete;
    FinalPass& operator=(const FinalPass&) = delete;

    /**
     * Draws `source` over the whole swapchain image, with `feed` showing through
     * wherever `source` is transparent.
     *
     * `source` must already be in SHADER_READ_ONLY_OPTIMAL (MeshPass leaves it
     * that way on its own barrier) and the swapchain image must already be a
     * colour attachment, which the frame loop arranged before calling the world.
     *
     * `feed` may be null, or may be non-null but have nothing uploaded yet. Both
     * mean the same thing here: the mesh pass will have cleared opaquely, so its
     * alpha is 1 everywhere and the feed contributes nothing whatever is bound.
     * */
    void Render(const FrameContext& frame, const Image& source, const CameraFeed* feed);

private:
    FinalPass() = default;

    /** Matches the block in fullscreen.vert and composite.frag. std430 packing. */
    struct PushConstants {
        float cameraUv[6] = {};
        int32_t vFirst = 0;
        int32_t srgbTarget = 0;
    };

    /** Points the descriptors at new images, waiting for the GPU first. */
    bool PointAt(const Image& source, VkImageView luma, VkImageView chroma, VkSampler feedSampler);

    /**
     * Gets the 1x1 stand-in into a samplable layout, once.
     *
     * It exists because a descriptor must name a valid view even on the frames
     * where nothing samples it in earnest: before the first camera image, or
     * forever on a device with no ARCore. Binding it is cheaper than a second
     * pipeline, and cheaper than a branch in the shader that would run per pixel
     * to avoid a fetch that is already multiplied by zero.
     * */
    void EnsurePlaceholder(const FrameContext& frame);

    VkDevice handle_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    std::unique_ptr<Image> placeholder_;
    bool placeholderReady_ = false;

    PushConstants push_;

    /** Which views the descriptors currently name, so they are only rewritten on a change. */
    VkImageView boundView_ = VK_NULL_HANDLE;
    VkImageView boundLuma_ = VK_NULL_HANDLE;
    VkImageView boundChroma_ = VK_NULL_HANDLE;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_FINALPASS_H
