#ifndef PUTORANA_GRAPHICS_FINALPASS_H
#define PUTORANA_GRAPHICS_FINALPASS_H

#include "Image.h"
#include "World.h"
#include "volk.h"

#include <memory>
#include <string>

namespace putorana::graphics {

class Device;

/**
 * Puts what the mesh pass drew onto the image that gets presented, by drawing
 * one screen-covering triangle that samples it.
 *
 * ## Why this exists at all
 *
 * The mesh pass could draw straight into the swapchain image and save a full
 * screen write plus a full screen read — which on a phone is real bandwidth, and
 * this pass is not free. It is here because everything that has to happen to a
 * finished image happens in it: tone mapping the day the mesh pass draws into a
 * float target and light is allowed past 1.0, and any post effect after that.
 * Those are not things a geometry pass can do to itself, because they need the
 * whole image and it is still being written.
 *
 * ## Its own set 0
 *
 * Nothing here shares the mesh pass's descriptor layouts. There is no camera, no
 * per-object array and no material — one binding, one combined image sampler,
 * and it is set 0 because it is the only set there is. A pass gets to define its
 * own numbering; the three-set contract in Material.h is about shaders that draw
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
     * Draws `source` over the whole swapchain image.
     *
     * `source` must already be in SHADER_READ_ONLY_OPTIMAL — MeshPass leaves it
     * that way on its own barrier — and the swapchain image must already be a
     * colour attachment, which the frame loop arranged before calling the world.
     * */
    void Render(const FrameContext& frame, const Image& source);

private:
    FinalPass() = default;

    /** Points the descriptor at a new image, waiting for the GPU first. */
    bool PointAt(const Image& source);

    VkDevice handle_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    /** Which view the descriptor currently names, so it is only rewritten on a resize. */
    VkImageView boundView_ = VK_NULL_HANDLE;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_FINALPASS_H
