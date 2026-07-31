#ifndef PUTORANA_GRAPHICS_MATERIAL_H
#define PUTORANA_GRAPHICS_MATERIAL_H

#include "Mesh.h"
#include "volk.h"

namespace putorana::graphics {

/**
 * The descriptor set numbering, shared by every shader that draws a mesh. It is
 * a contract, not a convention: a pipeline layout lists its sets in this order,
 * and getting it wrong is a validation error rather than a wrong picture.
 *
 * The split is by UPDATE FREQUENCY, which is the only thing set numbering is
 * good for. Set 0 is written once a frame and bound once; set 1 likewise; set 2
 * changes as the sorted draw list walks from material to material. Vulkan
 * disturbs the sets NUMBERED HIGHER than one whose layout changed, so putting
 * the most volatile last is what keeps a material switch from invalidating the
 * camera.
 * */
enum : uint32_t {
    /** Frame globals: camera now, lights when there are any. Owned by the pass. */
    kFrameSet = 0,
    /** Per-object data indexed by gl_InstanceIndex — the instancing. Owned by the pass. */
    kObjectSet = 1,
    /** This material instance: its parameters, its textures. Owned by the material. */
    kMaterialSet = 2,
};

/**
 * Everything a material needs from the outside world to build a pipeline. The
 * render pass fills it in, because every field is something the PASS decides:
 * what it draws into, and the layout of the two sets it owns.
 * */
struct PipelineContext {
    VkDevice device = VK_NULL_HANDLE;

    /**
     * The formats of the pass's attachments. With dynamic rendering there is no
     * VkRenderPass object to be compatible with, so a pipeline declares these
     * directly through VkPipelineRenderingCreateInfo — which every
     * implementation of CreatePipeline has to chain into pNext, or the pipeline
     * will not match anything vkCmdBeginRendering sets up.
     * */
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    VkDescriptorSetLayout frameLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout objectLayout = VK_NULL_HANDLE;
};

/**
 * A pipeline and the layout it was built with, which always travel together:
 * vkCmdBindDescriptorSets needs the layout, and in Vulkan — unlike WebGPU — the
 * pipeline does not carry it to the binding call.
 *
 * Owned by the render pass that asked for it, never by the material. See the
 * note on lifetime in the Material comment.
 * */
struct MaterialPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

/**
 * The link between a Renderable and the GPU, in two halves with two lifetimes.
 *
 *   The TYPE of material (the subclass) decides the PIPELINE: shader, blend,
 *   cull, depth state. One per (subclass, vertex format, pass), shared by every
 *   instance — which is what lets the pass sort draws by pipeline and switch
 *   only when the sorted list crosses a boundary.
 *
 *   The INSTANCE decides SET 2: its uniform buffer, its textures, its descriptor
 *   set. Two instances of one subclass differ here and nowhere else.
 *
 * ## Who owns the pipeline, and why it is not this class
 *
 * The renderer this is ported from caches pipelines in a static member of each
 * material class, with a comment noting the cache assumes a single device for
 * the life of the application. On Android that assumption is false: the VkDevice
 * is destroyed every time the app is backgrounded and built again on the way
 * back. A static cache would survive that and hand out pipelines belonging to a
 * device that no longer exists — which is not a crash at the point of the
 * mistake, it is a crash later, somewhere else.
 *
 * So the pass owns the pipelines. It has exactly the right lifetime (it dies
 * with the world, which dies with the device), it is where the attachment
 * formats come from, and it makes a second pass over the same materials — a
 * shadow pass, which wants depth-only pipelines from the same shaders — a
 * matter of a second cache rather than a redesign. The material only BUILDS the
 * pipeline; it never keeps it.
 *
 * ## What an implementation must do
 *
 *  - chain VkPipelineRenderingCreateInfo with the context's formats;
 *  - list the three set layouts in order: frame, object, then its own;
 *  - declare VIEWPORT and SCISSOR dynamic. The pass sets them per frame, because
 *    the surface can be resized or rotated under a pipeline that was built long
 *    before, and a baked viewport would then be wrong with no diagnostic;
 *  - read the vertex input from VertexInputFor(format), so one material can
 *    serve both a static and a skinned mesh.
 * */
class Material {
public:
    virtual ~Material();

    /**
     * Builds a pipeline for this material type and vertex format. Called once
     * per (type, format) by the pass's cache, never per instance and never per
     * frame; the pass takes ownership of both handles returned.
     *
     * Returning a null pipeline means failure — the pass logs and skips those
     * draws rather than binding nothing.
     *
     * const because it must not depend on this instance's parameters: two
     * instances of one subclass share the result, so anything that changes
     * render state has to be a different subclass. That is the same rule the
     * TypeScript version states, enforced here by the signature.
     * */
    virtual MaterialPipeline CreatePipeline(const PipelineContext& context,
                                            VertexFormat format) const = 0;

    /** Set 2 for this instance. Allocated from the device's DescriptorPool. */
    virtual VkDescriptorSet descriptorSet() const = 0;

protected:
    Material() = default;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_MATERIAL_H
