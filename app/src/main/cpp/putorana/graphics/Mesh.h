#ifndef PUTORANA_GRAPHICS_MESH_H
#define PUTORANA_GRAPHICS_MESH_H

#include "Buffer.h"
#include "FrameRing.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace putorana::graphics {

/**
 * Two independent axes, and keeping them independent is the whole design.
 *
 *   VertexFormat  — what a vertex contains. Changes the stride and the vertex
 *                   input state a pipeline is built with. Nothing else.
 *   MeshStorage   — whether the geometry can be rewritten after setup. Changes
 *                   how the memory is laid out. Nothing else.
 *
 * Every combination works, and neither axis knows the other exists.
 * */
enum class VertexFormat {
    Static,
    Skinned,
    /**
     * Position and normal, nothing else. For geometry that is generated rather
     * than authored — the reconstruction's chunk meshes — where there is no
     * texture and therefore no UV to store.
     *
     * It exists because the alternative was writing 8 bytes of zeroes per vertex
     * that no shader ever reads. That is 25% of the largest buffer in the app: a
     * room at 4cm voxels is on the order of a million reconstructed vertices, so
     * the UV lane alone would be ~8 MB of a phone's budget, plus its share of
     * every byte of upload traffic on every remesh.
     *
     * The cost is a second pipeline, because vertex input layout is baked into
     * one. That in turn means a second material with a good deal of duplication
     * against FlatColorMaterial — a deliberate, known debt to be paid off by
     * factoring the common pipeline setup out later, not by carrying dead lanes
     * forever.
     * */
    PositionNormal,
};

enum class MeshStorage {
    /**
     * Written once, at creation. One copy of the data, and the GPU may read it
     * on any frame forever.
     * */
    Immutable,
    /**
     * Rewritable per frame. Costs kFramesInFlight copies of the geometry — see
     * the note on N-buffering in the Mesh comment.
     * */
    Mutable,
};

/**
 * 32 bytes. Position first because every consumer that only wants positions
 * (bounds, culling, a depth-only pass) can then use one stride and offset 0
 * regardless of format.
 * */
struct StaticVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};
static_assert(sizeof(StaticVertex) == 32, "StaticVertex must stay tightly packed at 32 bytes");

/**
 * 24 bytes. The generated-geometry format — see VertexFormat::PositionNormal.
 *
 * Position and normal sit at the same offsets as StaticVertex's, deliberately.
 * Anything that only wants those two (bounds, culling, a depth-only pass) reads
 * both formats with the same offsets and only a different stride, which is the
 * same invariant the static_asserts in Mesh.cpp protect for SkinnedVertex.
 * */
struct PositionNormalVertex {
    glm::vec3 position;
    glm::vec3 normal;
};
static_assert(sizeof(PositionNormalVertex) == 24,
              "PositionNormalVertex must stay tightly packed at 24 bytes");

/**
 * 52 bytes: the static layout, a quad of joint indices and four float weights.
 *
 * Weights are stored exactly as the exporter produced them — R32G32B32A32_SFLOAT
 * and nothing else. They are normalised floats in the asset and they stay
 * normalised floats here, so what the skinning shader multiplies by is bit for
 * bit what the artist's rig produced.
 *
 * Joint indices are R8G8B8A8_UINT, which is what glTF itself uses for JOINTS_0
 * (unsigned byte or unsigned short) and caps a mesh at 256 joints — past any
 * real character rig, and a mesh that needed more would be split for other
 * reasons anyway. The format carries mandatory VERTEX_BUFFER support in the
 * Vulkan spec, so there is nothing to query at runtime, and it arrives in the
 * shader as a uvec4.
 *
 * Skinning is always four influences. glTF's JOINTS_0/WEIGHTS_0 is exactly four,
 * and a second set is a problem for the day a model needs one.
 * */
struct SkinnedVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    uint8_t boneIds[4];
    glm::vec4 weights;
};
static_assert(sizeof(SkinnedVertex) == 52, "SkinnedVertex must stay tightly packed at 52 bytes");
static_assert(alignof(SkinnedVertex) == 4,
              "SkinnedVertex must not pick up 16-byte alignment from glm::vec4 — the stride and "
              "every region offset in this file assume 4");

/** Bytes per vertex for a format. */
uint32_t VertexStrideFor(VertexFormat format);

/**
 * The format's name, for diagnostics. Exists so that a log line naming a format
 * cannot drift out of date when a format is added — which it silently did the
 * first time, calling everything that was not skinned "static".
 * */
const char* VertexFormatName(VertexFormat format);

/**
 * Axis-aligned bounding box. Local space when it comes off a Mesh, world space
 * once a Renderable has pushed it through a node's transform.
 * */
struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/**
 * Everything a pipeline needs to know about how to feed a vertex shader, as a
 * value type.
 *
 * It owns its arrays on purpose. VkPipelineVertexInputStateCreateInfo is nothing
 * but two pointers and two counts, so building it from temporaries is the single
 * most common way to hand vkCreateGraphicsPipelines dangling memory — the bug
 * shows up as garbage geometry on one driver and works fine on another. Keep the
 * VertexInput alive across the vkCreateGraphicsPipelines call and it cannot
 * happen.
 * */
struct VertexInput {
    std::array<VkVertexInputBindingDescription, 1> bindings{};
    std::array<VkVertexInputAttributeDescription, 5> attributes{};
    uint32_t attributeCount = 0;

    /** Points into *this*. Must not outlive the VertexInput it came from. */
    VkPipelineVertexInputStateCreateInfo CreateInfo() const;
};

/**
 * The vertex input state for a format. Shader locations are fixed and shared by
 * both formats, so a shader written against Static reads correctly from a
 * Skinned mesh and simply ignores locations 3 and 4:
 *
 *   0 position   1 normal   2 uv   3 boneIds   4 weights
 * */
VertexInput VertexInputFor(VertexFormat format);

struct MeshDesc {
    /** For VMA's stats dump and for error messages. */
    std::string name;

    VertexFormat format = VertexFormat::Static;
    MeshStorage storage = MeshStorage::Immutable;

    /**
     * Interleaved vertices in the layout `format` names — StaticVertex or
     * SkinnedVertex. Required for Immutable; may be null for Mutable, which then
     * starts empty and waits for the first Update.
     * */
    const void* vertices = nullptr;
    uint32_t vertexCount = 0;

    /**
     * Always uint32 on this side of the fence, whatever the mesh ends up storing
     * — Mesh narrows to uint16 when it can. One index type for every caller
     * means loaders never have to think about it.
     * */
    const uint32_t* indices = nullptr;
    uint32_t indexCount = 0;

    /**
     * Mutable only: the most this mesh will ever hold. Zero means "exactly the
     * initial counts". Capacity is what fixes the index type and the region
     * size, so a mutable mesh that will grow must declare it up front.
     * */
    uint32_t vertexCapacity = 0;
    uint32_t indexCapacity = 0;
};

/**
 * Indexed geometry on the GPU: one vertex buffer, one index buffer, and the
 * counts to draw them with. Knows nothing about materials, pipelines or the
 * scene graph — a Mesh is a thing you bind, not a thing that draws itself.
 *
 * On mutability and why it costs memory rather than a staging buffer.
 *
 * The Android part of the answer is the easy part: memory is unified, so an
 * update is a memcpy into a mapped pointer and there is no transfer to schedule.
 * What that does not solve is timing. kFramesInFlight frames are in flight at
 * once, so while frame N is being recorded on the CPU, the GPU is still reading
 * the buffers frame N-1 bound. Overwriting them is a data race with no
 * validation layer message and a symptom — geometry flickering between two
 * shapes — that looks like a bug in whatever generated the vertices.
 *
 * The fix is one region per frame in flight, inside the same allocation, and the
 * frame index picking which one to write and bind. That is why Bind, Draw and
 * Update all take a frameIndex: pass FrameSlot::index and it is correct by
 * construction. An Immutable mesh has a single region and ignores the argument
 * entirely, so callers never branch on storage type.
 *
 * The contract that falls out of it: whatever writes a Mutable mesh writes the
 * WHOLE region it is about to draw, every frame it changes. Data written to one
 * slot is not in the others. Initial data given to Create is written to all of
 * them, so a mutable mesh that is never updated still draws what it was born
 * with.
 * */
class Mesh {
public:
    static std::unique_ptr<Mesh> Create(VmaAllocator allocator, const MeshDesc& desc,
                                        std::string& error);

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    const std::string& name() const { return name_; }
    VertexFormat format() const { return format_; }
    MeshStorage storage() const { return storage_; }

    /**
     * Binds this mesh's vertex and index buffers at the offsets for `frameIndex`
     * (FrameSlot::index). The only command this class records, and it is here
     * only because the region layout behind those offsets is nobody else's
     * business.
     * */
    void Bind(VkCommandBuffer commandBuffer, uint32_t frameIndex) const;

    /**
     * Indices to draw for this frame, for the caller's own vkCmdDrawIndexed.
     *
     * There is deliberately no Draw() on this class. A draw is not a property of
     * the geometry: instanceCount and firstInstance come from a run of objects
     * that happen to share a pipeline, a material and this mesh, which is
     * knowledge that only the render pass doing the batching has. A Draw() here
     * would have to take both as arguments anyway, and it would hide this count
     * — the very number the batching loop needs — inside itself, quietly
     * inviting one draw call per object.
     *
     * Zero means the region has not been written yet, which only a Mutable mesh
     * created without initial data can be. Skip it.
     * */
    uint32_t indexCount(uint32_t frameIndex) const { return indexCounts_[RegionFor(frameIndex)]; }

    /**
     * Replaces the geometry in `frameIndex`'s region. Mutable meshes only;
     * refused and logged on an Immutable one.
     *
     * Must run before the command buffer that binds this region is submitted,
     * which on the render thread means during the frame's update, not after
     * recording. Recomputes the bounds from the new vertices.
     * */
    bool Update(uint32_t frameIndex, const void* vertices, uint32_t vertexCount,
                const uint32_t* indices, uint32_t indexCount);

    /**
     * Local-space AABB of the geometry, from the positions themselves. Position
     * sits at offset 0 in every format, so this is one loop for all of them.
     * Frustum culling and any spatial structure start here; for the world-space
     * box, see Renderable::WorldBounds.
     *
     * On a Mutable mesh this describes the most recent Update.
     * */
    const Aabb& bounds() const { return bounds_; }

    VkIndexType indexType() const { return indexType_; }
    uint32_t vertexCapacity() const { return vertexCapacity_; }
    uint32_t indexCapacity() const { return indexCapacity_; }

private:
    Mesh() = default;

    /** Immutable meshes collapse every frame index onto region 0. */
    uint32_t RegionFor(uint32_t frameIndex) const {
        return storage_ == MeshStorage::Immutable ? 0 : frameIndex % regionCount_;
    }

    /** Shared by Create and Update. Returns false with the reason logged. */
    bool WriteRegion(uint32_t region, const void* vertices, uint32_t vertexCount,
                     const uint32_t* indices, uint32_t indexCount);

    void ComputeBounds(const void* vertices, uint32_t vertexCount);

    std::string name_;
    VertexFormat format_ = VertexFormat::Static;
    MeshStorage storage_ = MeshStorage::Immutable;

    uint32_t vertexStride_ = 0;
    VkIndexType indexType_ = VK_INDEX_TYPE_UINT16;
    uint32_t vertexCapacity_ = 0;
    uint32_t indexCapacity_ = 0;

    /** 1 when Immutable, FrameRing::kFramesInFlight when Mutable. */
    uint32_t regionCount_ = 1;
    VkDeviceSize vertexRegionSize_ = 0;
    VkDeviceSize indexRegionSize_ = 0;

    /** Per region, because a Mutable mesh may draw a different count each frame. */
    std::array<uint32_t, FrameRing::kFramesInFlight> indexCounts_{};

    std::unique_ptr<Buffer> vertexBuffer_;
    std::unique_ptr<Buffer> indexBuffer_;

    Aabb bounds_;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_MESH_H
