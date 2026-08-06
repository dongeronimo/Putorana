#ifndef PUTORANA_GRAPHICS_MESHPASS_H
#define PUTORANA_GRAPHICS_MESHPASS_H

#include "Buffer.h"
#include "FrameRing.h"
#include "Image.h"
#include "Material.h"
#include "World.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace putorana::graphics {

class Device;
class Node;

/**
 * Set 0, binding 0. One per frame, written before anything is recorded.
 *
 * view and proj stay separate rather than pre-multiplied: shadowing and
 * lighting want them individually (the camera position, the view space) so no
 * shader receives a viewProj it cannot take apart.
 *
 * Lights belong here too, and are not here yet. Adding them changes the layout
 * of set 0, and set 0's layout is shared by every material's pipeline layout, so
 * it is a change that rebuilds every pipeline at once. Worth knowing before it
 * looks like a small edit.
 * */
struct FrameData {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    /** World-space camera position. w is padding; std140 aligns a vec3 to 16 anyway. */
    glm::vec4 cameraPosition{0.0f};
};

/**
 * Set 1, one element per draw, indexed in the shader by gl_InstanceIndex.
 *
 * This is the whole instancing mechanism. Every visible object gets a slot, the
 * slots are filled in the SAME order the draws are issued, and a draw's
 * firstInstance is the index of its first slot, so gl_InstanceIndex, which
 * Vulkan defines as firstInstance plus the instance counter, lands exactly on
 * the right one. A run of objects sharing pipeline, material and mesh therefore
 * collapses into one vkCmdDrawIndexed no matter where each of them is.
 *
 * normalMatrix is transpose(inverse(model)) and not the model matrix, because
 * under non-uniform scale the plain inverse skews normals off the surface; the
 * inverse transpose is the one that keeps them perpendicular. mat4 and not mat3
 * only because a mat3 in std430 is padded to three vec4 regardless, so the
 * saving would be 16 bytes in exchange for an alignment trap.
 * */
struct ObjectData {
    glm::mat4 model{1.0f};
    glm::mat4 normalMatrix{1.0f};
};

/**
 * Draws the opaque geometry of a scene into its own colour and depth targets.
 *
 * Not a VkRenderPass. Dynamic rendering means there is no such object anywhere
 * in this renderer; "render pass" here is the logical unit: a set of targets, a
 * set of descriptor layouts, and an algorithm for turning a tree of nodes into
 * as few draw calls as it can.
 *
 * ## The four steps of a frame
 *
 *   1. COLLECT: walk the tree, keep renderables whose passMask has this pass's
 *      bit, resolve each one's pipeline through the cache.
 *   2. SORT: by pipeline, then material, then mesh. Legal here because the pass
 *      is entirely opaque and depth decides visibility; in a pass with blending
 *      the order would be correctness, not an optimisation.
 *   3. UPLOAD: write every object's matrices into set 1's buffer, in sorted
 *      order.
 *   4. DRAW: walk the sorted list in runs that share all three keys and emit
 *      one instanced draw per run.
 *
 * ## Why the pipeline cache lives here
 *
 * Pipelines depend on the material type, the vertex format AND this pass's
 * attachment formats, so the pass is the only place that knows the whole key.
 * More importantly it has the right lifetime: it dies with the world, which dies
 * with the Device. See the long note in Material.h about why a static cache,
 * which is what the WebGPU original uses, is a latent crash on Android.
 * */
class MeshPass {
public:
    /**
     * `colorFormat` is what the pass draws into and what the final pass will
     * sample. The depth format is chosen here from what the adapter supports.
     * */
    static std::unique_ptr<MeshPass> Create(Device& device, VkFormat colorFormat,
                                            std::string& error);

    ~MeshPass();

    MeshPass(const MeshPass&) = delete;
    MeshPass& operator=(const MeshPass&) = delete;

    /**
     * Records the pass. Targets are (re)built here if the extent changed, so a
     * resize or a rotation needs no separate call.
     *
     * `transparentClear` clears the colour target to zero alpha instead of an
     * opaque colour, which is what tells the final pass to show the camera feed
     * wherever no geometry was drawn. Pass false, the default, when there is
     * no feed to show, and the opaque clear then carries straight through the
     * final pass's mix unchanged.
     * */
    void Render(const FrameContext& frame, Node& root, bool transparentClear = false);

    /**
     * What the pass drew, left in SHADER_READ_ONLY_OPTIMAL for the final pass to
     * sample. Null until the first Render has built the targets.
     * */
    const Image* colorTarget() const { return color_.get(); }

    VkFormat colorFormat() const { return colorFormat_; }
    VkFormat depthFormat() const { return depthFormat_; }

private:
    MeshPass() = default;

    /** One entry per (material type, vertex format). */
    struct PipelineKey {
        std::type_index materialType;
        VertexFormat vertexFormat;

        bool operator==(const PipelineKey& other) const {
            return materialType == other.materialType && vertexFormat == other.vertexFormat;
        }
    };
    struct PipelineKeyHash {
        size_t operator()(const PipelineKey& key) const {
            return key.materialType.hash_code() ^ (static_cast<size_t>(key.vertexFormat) << 1);
        }
    };

    struct DrawItem {
        const Renderable* renderable = nullptr;
        const Material* material = nullptr;
        /**
         * Into pipelines_. A pointer and not a copy because it is also the sort
         * key, and unordered_map guarantees its elements never move.
         * */
        const MaterialPipeline* pipeline = nullptr;
        const Mesh* mesh = nullptr;
        const glm::mat4* worldMatrix = nullptr;
    };

    bool CreateLayouts(std::string& error);
    bool CreateFrameResources(std::string& error);
    bool EnsureTargets(VkExtent2D extent);
    bool EnsureObjectCapacity(uint32_t objectCount);

    const MaterialPipeline* PipelineFor(const Material& material, VertexFormat format);

    void Collect(Node& node, std::vector<DrawItem>& items, const Node*& cameraNode);

    Device* device_ = nullptr;
    VkDevice handle_ = VK_NULL_HANDLE;

    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    std::unique_ptr<Image> color_;
    std::unique_ptr<Image> depth_;

    VkDescriptorSetLayout frameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout objectLayout_ = VK_NULL_HANDLE;

    // Per frame in flight, for the same reason a mutable Mesh is: these are
    // rewritten from the CPU every frame while the GPU may still be reading the
    // previous frame's contents.
    std::array<std::unique_ptr<Buffer>, FrameRing::kFramesInFlight> frameBuffers_{};
    std::array<VkDescriptorSet, FrameRing::kFramesInFlight> frameSets_{};
    std::array<std::unique_ptr<Buffer>, FrameRing::kFramesInFlight> objectBuffers_{};
    std::array<VkDescriptorSet, FrameRing::kFramesInFlight> objectSets_{};
    uint32_t objectCapacity_ = 0;

    std::unordered_map<PipelineKey, MaterialPipeline, PipelineKeyHash> pipelines_;

    /** Reused across frames so a steady scene allocates nothing per frame. */
    std::vector<DrawItem> items_;
    std::vector<ObjectData> objectStaging_;

    /** So a scene with no camera says so once instead of sixty times a second. */
    bool warnedAboutCamera_ = false;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_MESHPASS_H
