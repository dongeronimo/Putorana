#ifndef PUTORANA_GRAPHICS_FLATCOLORMATERIAL_H
#define PUTORANA_GRAPHICS_FLATCOLORMATERIAL_H

#include "Buffer.h"
#include "Material.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace putorana::graphics {

class Device;

/**
 * The first concrete material: one colour, one hardcoded light direction.
 *
 * Not quite the unlit material the WebGPU renderer starts with, and the
 * difference is deliberate — see the comment in mesh_flat.frag. A rotating cube
 * in flat colour is a hexagon, and the whole job of the first thing that draws
 * is to show that winding, normals and depth order are arriving correctly.
 *
 * It is also the worked example of the Material contract: what to chain, what to
 * declare dynamic, and where the set layouts come from. The next material is a
 * copy of this file with a different shader and a different parameter block.
 * */
class FlatColorMaterial : public Material {
public:
    static std::unique_ptr<FlatColorMaterial> Create(Device& device, const glm::vec4& color,
                                                     std::string& error);

    ~FlatColorMaterial() override;

    void SetColor(const glm::vec4& color);

    MaterialPipeline CreatePipeline(const PipelineContext& context,
                                    VertexFormat format) const override;

    VkDescriptorSet descriptorSet() const override { return descriptorSet_; }

private:
    FlatColorMaterial() = default;

    VkDevice handle_ = VK_NULL_HANDLE;

    /**
     * Set 2's layout, owned per INSTANCE rather than per class.
     *
     * Two instances therefore create two identical layout objects, which is
     * fine: Vulkan's pipeline layout compatibility is defined on layouts being
     * IDENTICALLY DEFINED, not on handle identity, so a pipeline built from one
     * instance's layout binds another instance's descriptor set. The alternative
     * — one static layout for the class — would outlive the VkDevice it was
     * created from, which on Android is destroyed on every trip to background.
     * */
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> params_;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_FLATCOLORMATERIAL_H
