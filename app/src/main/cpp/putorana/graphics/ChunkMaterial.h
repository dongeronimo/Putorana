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
 * The reconstruction's material: the camera colour each vertex carries, over a
 * flat lit colour wherever there is no camera colour yet. Drawn from
 * VertexFormat::Reconstructed geometry.
 *
 * ## Why this is a copy of FlatColorMaterial rather than a reuse of it
 *
 * Two reasons, and neither is avoidable by being cleverer with this class.
 *
 * The shader cannot be shared. mesh_flat.vert declares `layout(location = 2) in
 * vec2 inUv`, and a pipeline built for Reconstructed supplies locations 0, 1 and
 * 5 — the shader would read an attribute the pipeline never binds. So chunk
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

    /**
     * The colour drawn where the reconstruction has geometry but no camera
     * colour to put on it. Lit by the hardcoded directional light.
     * */
    void SetColor(const glm::vec4& color);

    /**
     * How far to trust the per-vertex camera colour, 0 to 1.
     *
     * Multiplied by each vertex's own confidence byte, so 1 means "use the
     * camera colour wherever there is one" and 0 means "ignore it everywhere".
     *
     * Zero is the debugging setting, and it is worth knowing it exists before
     * the first time the reconstruction looks wrong. Camera colour is drawn
     * essentially unlit, because it already contains the room's real lighting,
     * and unlit colour hides exactly the faults a single directional light was
     * chosen to expose: a surface that is noisy, inside out, or facing the wrong
     * way. Setting this to 0 puts that light back and answers whether the
     * problem is the geometry or the colour on it.
     * */
    void SetColorMix(float mix);

    /**
     * Whether the attachment this material draws into is an _SRGB format, which
     * decides whether the shader hands over linear or gamma-encoded values.
     *
     * Not a property of the material and it should not have to be here. It is,
     * because the per-vertex colour came off a camera in gamma-encoded sRGB and
     * has to end up in whatever encoding the attachment expects, and a material
     * has no way to ask the pass what that is: the format arrives in
     * CreatePipeline, long after the uniform buffer was written.
     *
     * Whoever creates the pass tells the material, with IsSrgbFormat from
     * Swapchain.h. Getting it wrong puts the reconstruction at a visibly
     * different brightness from the camera feed immediately behind it, which is
     * the one comparison this app makes on every frame.
     * */
    void SetSrgbTarget(bool srgb);

    MaterialPipeline CreatePipeline(const PipelineContext& context,
                                    VertexFormat format) const override;

    VkDescriptorSet descriptorSet() const override { return descriptorSet_; }

private:
    ChunkMaterial() = default;

    /** Pushes both parameters below into the uniform buffer. */
    void Write();

    VkDevice handle_ = VK_NULL_HANDLE;

    glm::vec4 color_{1.0f};
    float colorMix_ = 1.0f;
    bool srgbTarget_ = true;

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
