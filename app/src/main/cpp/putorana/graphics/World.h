#ifndef PUTORANA_GRAPHICS_WORLD_H
#define PUTORANA_GRAPHICS_WORLD_H

#include "Material.h"
#include "Mesh.h"
#include "Node.h"
#include "volk.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace putorana::graphics {

class Device;
class Swapchain;

/**
 * Everything a world needs to record one frame. Assembled by the frame loop and
 * handed to World::Render.
 * */
struct FrameContext {
    /** Already begun, and already inside no render pass. The world records into it. */
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    /**
     * FrameSlot::index: which of the kFramesInFlight sets of per-frame resources
     * this frame owns. Goes straight into Mesh::Bind and into any buffer the
     * world writes from the CPU while the GPU may still be reading the previous
     * frame's copy.
     * */
    uint32_t frameIndex = 0;

    /**
     * Where the frame is presented. The colour format, the extent and the
     * preTransform the camera needs all come from here, so passing the swapchain
     * rather than four loose fields means they cannot be mismatched.
     *
     * It is replaced on every resize and rotation. A world that keeps render
     * targets of its own is responsible for noticing the extent changed and
     * rebuilding them — there is no callback for it.
     * */
    const Swapchain* swapchain = nullptr;

    /** The image acquired for this frame, already in COLOR_ATTACHMENT_OPTIMAL. */
    uint32_t imageIndex = 0;
};

/**
 * A world is a scene plus the way that scene is drawn. It owns four things and
 * that ownership is the whole point of the class:
 *
 *  - the scene tree, rooted at a node called ROOT;
 *  - the assets in it — meshes, materials;
 *  - its own sequence of render passes, because each world draws differently
 *    and the chain therefore cannot live in the frame loop;
 *  - the frame's update, which walks the tree once.
 *
 * ## Lifetime: nested inside the Device's
 *
 * The Device dies every time the app goes to background and is rebuilt on the
 * way back, so the world dies with it — its meshes are VMA allocations from an
 * allocator that is about to stop existing. Device holds the unique_ptr and
 * releases it first in its teardown, which is what makes the ordering
 * unfixable rather than a rule somebody has to remember.
 *
 * The practical consequence: there is no "load the assets once at startup".
 * Whatever loads them is inside CreateWorld, and CreateWorld runs again on
 * every return from background.
 *
 * ## The two-phase construction
 *
 *   1. construct         — stores the device, makes ROOT. Cheap, cannot fail.
 *   2. CreateRenderPasses — HOW this world draws. Needs the swapchain, so it
 *                           cannot happen before there is one.
 *   3. CreateWorld        — WHAT it draws: assets, materials, camera, lights.
 *   4. per frame          — Update(dt), then Render(context).
 *   5. destruction        — releases the lot.
 *
 * Two virtuals rather than one because the two halves answer different
 * questions and fail for different reasons: a broken pipeline is a shader
 * problem, a missing model is an asset problem, and a single Initialize would
 * report both as "world failed".
 * */
class World {
public:
    virtual ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /** The scene tree. Render passes walk from here. */
    Node& root() const { return *root_; }

    /**
     * First node with this name, or null. O(n) — if it ever hurts, the world
     * keeps a map. It is a read path for debugging and for tools, deliberately
     * by name rather than by cached Node*: a cached pointer dangles the moment
     * the surface is lost and the world is rebuilt.
     * */
    Node* FindNode(const std::string& name) const;

    /**
     * Hands a mesh to the world, which owns it from now on, and returns a
     * non-owning pointer for the Renderables that will share it.
     *
     * Assets are owned here and not by the nodes because they are SHARED: two
     * hundred wall tiles point at one mesh. The TypeScript renderer this comes
     * from leaves the mesh list to each concrete world, which means every new
     * world can forget to destroy them; here the base owns it and there is
     * nothing to forget.
     * */
    Mesh* AddMesh(std::unique_ptr<Mesh> mesh);

    /**
     * Same for materials, but keyed, because the loader resolves them by the
     * name in a Blender custom property.
     *
     * A member and not the global registry the TypeScript version uses. That
     * registry has to be emptied on every world change or the next world
     * inherits orphans, and forgetting leaks the buffers of the old one. Scoped
     * to the world, the problem does not exist.
     *
     * Returns null if the name is already taken — replacing silently would leave
     * live renderables pointing at a destroyed material.
     * */
    Material* AddMaterial(std::string name, std::unique_ptr<Material> material);

    Material* FindMaterial(const std::string& name) const;

    /**
     * Queues `node` and its whole subtree for removal at the END of the current
     * Update. Deferred and not immediate because the traversal is walking the
     * very lists this would erase from.
     *
     * Queueing the same node twice is harmless, and so is queueing both a node
     * and one of its descendants — the flush drops anything already covered by
     * an ancestor rather than freeing it twice.
     *
     * Does not destroy meshes or materials: those are shared and stay the
     * world's.
     * */
    void DestroyNode(Node& node);

    /**
     * One frame of simulation. Walks the tree once, top-down, refreshing every
     * node's cached world matrix, then flushes whatever DestroyNode queued.
     *
     * The traversal lives here rather than in Node because of what will share
     * it: each node's behaviours run BEFORE that node's matrix closes, so a
     * behaviour moving its own node — or a descendant — takes effect in the same
     * frame, while touching an ANCESTOR whose matrix already closed shows up in
     * the next one. That ordering is a design decision, and it belongs to
     * whoever owns the frame.
     *
     * Call before Render, every frame.
     *
     * Virtual so a world can do its own per-frame work, and an override MUST
     * call this base — skipping it leaves every world matrix at the previous
     * frame's value and nothing appears to move. Do that work BEFORE the base
     * call: it is what closes the matrices, so anything moved afterwards lands a
     * frame late.
     *
     * This is the seam behaviours will replace. Until they exist it is the only
     * place a world can animate anything.
     * */
    virtual void Update(float deltaSeconds);

    /**
     * Builds HOW this world draws: its render passes and the wiring between
     * them. Called once, after there is a swapchain, before CreateWorld.
     *
     * Returns false with the reason in `error`; the frame loop then draws
     * nothing rather than half a world.
     * */
    virtual bool CreateRenderPasses(const Swapchain& swapchain, std::string& error) = 0;

    /** Builds WHAT it draws: assets, materials, the camera, the lights. */
    virtual bool CreateWorld(std::string& error) = 0;

    /**
     * Records this world's passes into the frame's command buffer, in the order
     * this world wants them. The frame loop does the acquire, the layout
     * transitions and the submit, and knows nothing about which passes exist.
     * */
    virtual void Render(const FrameContext& frame) = 0;

    /**
     * Valid for the whole life of the world, by construction. Public because a
     * loader or a pass needs the allocator and the descriptor pool as much as a
     * subclass does, and there is no invariant here to protect.
     * */
    Device& device() const { return device_; }

protected:
    explicit World(Device& device);

private:
    void FlushPendingDestruction();

    Device& device_;

    // The assets, declared FIRST so they are destroyed LAST. Members go away in
    // reverse declaration order, and the tree below holds non-owning pointers
    // into these two — a Renderable outliving its Mesh, even only for the length
    // of a destructor, is exactly what this ordering rules out.
    std::vector<std::unique_ptr<Mesh>> meshes_;
    std::unordered_map<std::string, std::unique_ptr<Material>> materials_;

    std::vector<Node*> pendingDestruction_;

    /**
     * Named ROOT and owned here. Destroying it destroys the entire scene, which
     * is the single delete the whole ownership design exists to make possible.
     *
     * Declared LAST on purpose: it is therefore destroyed FIRST, so the scene is
     * already gone by the time the assets it pointed at are.
     * */
    std::unique_ptr<Node> root_;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_WORLD_H
