#ifndef PUTORANA_GRAPHICS_RENDERABLE_H
#define PUTORANA_GRAPHICS_RENDERABLE_H

#include "Material.h"
#include "Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <type_traits>

namespace putorana::graphics {

/**
 * The passes a Renderable can take part in, one bit each.
 *
 * A bitmask rather than a single "which pass" field because one object really
 * does belong to several at once: the same crate is drawn by the main pass and
 * again by every shadow pass. Each pass walks the tree and skips whoever lacks
 * its bit.
 *
 * New bits get added when the pass that reads them is written, not before.
 * There are two here because there are two shapes of per-object data already
 * decided: a plain model matrix, and a block of bone matrices.
 * */
enum class RenderPassBit : uint32_t {
    /** The ordinary opaque geometry pass. */
    Main = 1u << 0,

    /**
     * Drawn by the skinned pass instead of the main one. INSTEAD, not as well.
     * The per-object descriptor set is a different shape (a pool of bone
     * matrices, not one model matrix), so a mesh cannot be in both.
     *
     * Note this is a separate question from VertexFormat::Skinned. A skinned
     * mesh can perfectly well be handed to the main pass and drawn rigid in its
     * bind pose; the format says what the vertices contain, the bit says who
     * draws them.
     * */
    Skinned = 1u << 1,
};

/**
 * An OR of RenderPassBit values.
 *
 * Plain uint32_t and not RenderPassBit, because the OR of two enumerators is
 * not itself an enumerator, and typing it as the enum would be a lie the compiler
 * happens to accept.
 * */
using RenderPassMask = uint32_t;

constexpr RenderPassMask operator|(RenderPassBit a, RenderPassBit b) {
    return static_cast<RenderPassMask>(a) | static_cast<RenderPassMask>(b);
}

constexpr RenderPassMask operator|(RenderPassMask a, RenderPassBit b) {
    return a | static_cast<RenderPassMask>(b);
}

/**
 * What a scene node draws. One Mesh, one Material, and the handful of per-object
 * decisions that are the object's to make rather than the asset's.
 *
 * It owns neither of them. Mesh and Material are GPU assets shared by reference:
 * the two hundred wall tiles of a dungeon point at one Mesh and one Material
 * between them, and the thing that destroys those assets is whatever loaded
 * them, which has to die with the Device. A Renderable is small, cheap and
 * plain: two pointers and two fields.
 *
 * Which is why it is copyable and why that matters. Instancing a prefab means
 * "same assets, own per-object state", and for this class the compiler-generated
 * copy constructor is exactly that: the pointers are shared, the mask and the
 * flag are copied. The TypeScript renderer this is ported from needs a
 * hand-written cloneRenderable for the same job, and it has a bug waiting in it
 * every time a field is added and the clone is not updated. Here there is
 * nothing to keep in sync.
 *
 * It knows nothing about Vulkan, and nothing about where it sits in the world:
 * WorldBounds takes the matrix as an argument rather than reaching for a node.
 * That is what keeps the scene graph and the renderer separable.
 * */
class Renderable {
public:
    /** The mesh must outlive the renderable. Nothing here checks that. */
    explicit Renderable(Mesh& mesh) : mesh_(&mesh) {}

    /**
     * Null until something assigns one: the loader builds renderables from a
     * glTF, which carries no material of ours. A pass that meets a null material
     * is expected to substitute a loud fallback rather than skip the draw: an
     * object screaming "I have no material" is far easier to debug than an
     * object that silently is not there.
     * */
    Material* material = nullptr;

    /**
     * Which passes draw this (an OR of RenderPassBit). Defaults to the main
     * pass, which is what an ordinary object wants.
     * */
    RenderPassMask passMask = static_cast<RenderPassMask>(RenderPassBit::Main);

    /**
     * Does this object CAST a shadow? Nothing reads it yet (there is no shadow
     * pass) but it belongs to the Renderable rather than to the Material, and
     * that placement is the decision worth recording now.
     *
     * It is a question about the object because two objects sharing one material
     * are allowed to disagree, and because it is the object that occupies space.
     * The case that forces it is ground clutter: grass, pebbles, debris,
     * geometry thinner than a shadow-map texel, which contributes no lighting
     * information and a great deal of artefact (hard speckle, aliasing, a shadow
     * detached from its own base by the depth bias), and costs a draw in every
     * shadow map besides. The occlusion such an object genuinely has, a darker
     * base, gets painted into the texture instead of computed.
     *
     * Not to be confused with RECEIVING shadow: something that casts none is
     * still shaded normally by everything that does.
     * */
    bool castsShadow = true;

    /**
     * The geometry. A reference and not a pointer at the call site because it is
     * never null; the constructor requires one.
     * */
    Mesh& mesh() const { return *mesh_; }

    /**
     * Forwarded from the mesh so a pass can pick the pipeline without asking
     * what kind of mesh it is holding. (The TypeScript version caches this in a
     * field to dodge an `instanceof`; here it is a member read through a
     * pointer, so there is nothing to cache.)
     * */
    VertexFormat vertexFormat() const { return mesh_->format(); }

    bool DrawnIn(RenderPassBit pass) const {
        return (passMask & static_cast<RenderPassMask>(pass)) != 0;
    }

    /**
     * The mesh's local AABB pushed through `worldMatrix`, as the enclosing
     * axis-aligned box in world space. Shared infrastructure: frustum culling
     * first, then whatever spatial structure the reconstruction needs.
     *
     * All eight corners, not just min and max, because rotation turns the box
     * and the two extreme corners of the local box are generally not the extreme
     * corners of the transformed one.
     *
     * `worldMatrix` must be affine, a node transform, which is what it will
     * always be. The result is conservative: a rotated box grows, and it never
     * shrinks back on its own.
     * */
    Aabb WorldBounds(const glm::mat4& worldMatrix) const;

private:
    /** Never null. Non-owning: the asset outlives every renderable using it. */
    Mesh* mesh_;
};

// The copy is the prefab clone; see the class comment. Pinned here so that
// adding a member that breaks it (a unique_ptr, a reference) fails the build
// instead of quietly turning instantiation into a compile error somewhere far
// away, or worse, into a deep copy of something that was meant to be shared.
static_assert(std::is_copy_constructible_v<Renderable> && std::is_copy_assignable_v<Renderable>,
              "Renderable must stay copyable: copying one IS instancing it");

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_RENDERABLE_H
