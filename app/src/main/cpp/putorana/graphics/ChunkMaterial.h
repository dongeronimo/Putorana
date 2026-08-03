#ifndef PUTORANA_GRAPHICS_CHUNKMATERIAL_H
#define PUTORANA_GRAPHICS_CHUNKMATERIAL_H

#include "Buffer.h"
#include "Material.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace putorana::graphics {

class Device;

/**
 * The reconstruction's material: one colour, one hardcoded light direction,
 * drawn from VertexFormat::PositionNormal geometry.
 *
 * ## Why this is a copy of FlatColorMaterial rather than a reuse of it
 *
 * Two reasons, and neither is avoidable by being cleverer with this class.
 *
 * The shader cannot be shared. mesh_flat.vert declares `layout(location = 2) in
 * vec2 inUv`, and a pipeline built for PositionNormal supplies only locations 0
 * and 1 — the shader would read an attribute the pipeline never binds. So chunk
 * geometry needs mesh_chunk.vert, and a different shader means a different
 * material.
 *
 * The pipeline cache cannot tell them apart otherwise. MeshPass keys cached
 * pipelines on (materialType, vertexFormat); two materials sharing a class but
 * loading different shaders would collide on that key and one would silently
 * draw with the other's pipeline.
 *
 * ## The seam, for whoever factors this out
 *
 * Everything below except the two shader paths and the class name is identical
 * to FlatColorMaterial, and it is ~130 lines of pipeline state that has nothing
 * to do with either material. The natural extraction is a helper taking
 * (PipelineContext, VertexFormat, vertex module, fragment module, set layout)
 * and returning a MaterialPipeline, leaving each material with its shader paths
 * and its parameter block. That is a refactor for when there is a third
 * material, not a second — with two, the duplication is still cheaper to read
 * than the abstraction would be.
 * */
class ChunkMaterial : public Material {
public:
    static std::unique_ptr<ChunkMaterial> Create(Device& device, const glm::vec4& color,
                                                 std::string& error);

    ~ChunkMaterial() override;

    void SetColor(const glm::vec4& color);

    MaterialPipeline CreatePipeline(const PipelineContext& context,
                                    VertexFormat format) const override;

    VkDescriptorSet descriptorSet() const override { return descriptorSet_; }

private:
    ChunkMaterial() = default;

    VkDevice handle_ = VK_NULL_HANDLE;

    /**
     * Set 2's layout, owned per INSTANCE. See the note in FlatColorMaterial.h:
     * Vulkan's layout compatibility is defined on identical DEFINITION rather
     * than handle identity, and a static layout would outlive the VkDevice it
     * came from — which Android destroys on every trip to background.
     * */
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> params_;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_CHUNKMATERIAL_H
