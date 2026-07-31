#ifndef PUTORANA_GRAPHICS_NODE_H
#define PUTORANA_GRAPHICS_NODE_H

#include "Camera.h"
#include "Light.h"
#include "Renderable.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace putorana::graphics {

/**
 * A transform in a hierarchy, plus whatever optional components are hung off it.
 *
 * A Node by itself is a position, a scale and an attitude. What it IS comes from
 * what is attached: a node with a renderable is something drawn, a node with a
 * camera is a camera, a node with a light is a light. Anything can carry any
 * combination, and a node carrying none is a pure pivot — which is not a
 * degenerate case but the most common one, since that is what every parent is.
 *
 * ## Rotation: quaternion inside, unbounded Euler outside
 *
 * The quaternion is the source of truth. It is what goes into the matrices and
 * what any future interpolation composes. The Euler angles are an EDITABLE VIEW
 * of that rotation, with the same semantics Unity gives them:
 *
 *  - Setting eulerAngles stores the numbers verbatim, with no clamping. Set
 *    (500, -720.5, 1234) and you read back (500, -720.5, 1234). That is what
 *    lets an animation drive an angle continuously through 360 without the value
 *    jumping.
 *  - Setting the quaternion directly makes those raw numbers meaningless — a
 *    quaternion does not remember how many turns you took to get there. The
 *    cache is flagged stale and the next read of eulerAngles derives canonical
 *    angles instead (x in [-90,90], y and z in [-180,180]).
 *
 * The Euler convention is degrees, applied Z then X then Y about WORLD axes, so
 * R = Ry * Rx * Rz. Unity's convention.
 *
 * position and scale are public and mutable in place, three.js style, because
 * nothing is derived from them. Rotation is not, because the Euler cache is.
 * That asymmetry is the whole reason one is a field and the other is a pair of
 * methods.
 *
 * ## Ownership
 *
 * A node owns its children, and a root is owned by whoever called Create.
 *
 * The alternative — every node a raw pointer, with one flat container owning
 * them all — is a real design and a common one, but it needs that container to
 * exist, and there is no World class yet. An owning tree needs nothing but
 * itself, and it fits this app's lifetime exactly: everything dies together when
 * the surface goes, so releasing the root releases the scene, with no traversal
 * and nothing left to leak.
 * */
class Node {
public:
    /**
     * Heap allocated, always. Private constructor because a node that ever gets
     * parented has to be owned by a unique_ptr, and a stack node that someone
     * later tries to AddChild would be a very quiet disaster.
     * */
    static std::unique_ptr<Node> Create(std::string name = {});

    ~Node() = default;

    // Neither copyable nor movable. Children hold a raw back-pointer to their
    // parent, so moving a node would leave every child pointing at a corpse.
    // Copying a subtree is a different operation with different semantics
    // (shared assets, fresh per-object state) and belongs to whatever implements
    // prefabs.
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    std::string name;

    // --- components -------------------------------------------------------
    //
    // Two different storage strategies here, and the reason is not taste.
    //
    // Renderable is a concrete, final, small value type. optional holds it
    // inline: no allocation, no indirection, and `if (node.renderable)` reads
    // the same as the TypeScript `if (node.renderable)`.
    //
    // Light and Camera cannot use optional. Both are polymorphic bases — the
    // actual object is a PointLight or a SpotLight, a PerspectiveCamera or an
    // ArCamera — and optional<Base> stores a Base by value, which would slice
    // the cone angles off a spot and ARCore's whole projection off a camera.
    // (Neither would even compile: both constructors are protected precisely so
    // that a bare base cannot exist.) Polymorphism means a pointer, so both are
    // unique_ptr, and null means the component is absent.

    /** Present means this node is drawn. */
    std::optional<Renderable> renderable;

    /**
     * Non-null means this node is a camera. The VIEW matrix is the inverse of
     * this node's world matrix — the camera looks down its own -Z, so the
     * transform below is the only thing that positions it.
     *
     * That holds for an ArCamera too, and deliberately: it writes ARCore's pose
     * into this node's transform rather than keeping a view matrix of its own,
     * so there is exactly one answer to "where is the camera" in the scene.
     * */
    std::unique_ptr<Camera> camera;

    /**
     * Non-null means this node is a light. Its world position and its -Z are
     * where the light's position and direction come from; see Light.h.
     * */
    std::unique_ptr<Light> light;

    // --- transform --------------------------------------------------------

    /** Local position, relative to the parent. Mutable in place. */
    glm::vec3 position{0.0f, 0.0f, 0.0f};

    /** Local scale, per axis. Mutable in place. */
    glm::vec3 scale{1.0f, 1.0f, 1.0f};

    /** Local rotation as a unit quaternion. */
    glm::quat rotation() const { return rotation_; }

    /**
     * Normalises on the way in, so everything downstream may assume a unit
     * quaternion, and invalidates the Euler cache — see the class comment.
     * */
    void SetRotation(const glm::quat& q);

    /**
     * Local rotation as Euler angles in degrees.
     *
     * Reading after setting gives back exactly what was set, unnormalised. If
     * the last change came through SetRotation instead, this derives canonical
     * angles from the quaternion.
     * */
    glm::vec3 eulerAngles() const;

    void SetEulerAngles(const glm::vec3& degrees);

    /**
     * Points this node's -Z at `target` (world coordinates), keeping +Y as close
     * to `up` as it can. Position is untouched.
     *
     * -Z because that is the glTF camera convention, which is what makes the
     * view matrix fall straight out of the inverse of the world matrix. (Note
     * this is the opposite of Unity, whose LookAt aims +Z.)
     *
     * The world-to-local step assumes no non-uniform scale anywhere up the
     * parent chain. Uniform scale is fine.
     * */
    void LookAt(const glm::vec3& target, const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

    /**
     * Copies the LOCAL transform of `src` — position, scale, rotation — keeping
     * the RAW Euler state, the continuous (500, -720) that going through
     * SetRotation would flatten. Touches neither the hierarchy nor the
     * components.
     * */
    void CopyLocalFrom(const Node& src);

    // --- hierarchy --------------------------------------------------------

    /** Null for a root. Non-owning: the parent owns this node, not the reverse. */
    Node* parent() const { return parent_; }

    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

    /**
     * Takes ownership of `child` and returns a non-owning pointer to it, so a
     * tree can be built and kept hold of in one expression:
     *
     *     Node* cube = root->AddChild(Node::Create("cube"));
     *
     * Returns null and logs if the child is this node or one of its ancestors,
     * which would build a cycle that owns itself and never dies. In that case
     * the child is destroyed along with its subtree — loudly, which beats a
     * graph that is quietly corrupt.
     * */
    Node* AddChild(std::unique_ptr<Node> child);

    /**
     * Moves this node under `newParent`, carrying its subtree along. The normal
     * way to reparent.
     *
     * Returns false, changing nothing, in two cases. One is a cycle — parenting
     * a node to its own descendant. The other is calling it on a node that is
     * still a root: its unique_ptr is held by whoever created it, and this
     * method has no way to take it away from them. Spell that one as
     *
     *     newParent.AddChild(std::move(theRootsPointer));
     *
     * which is the only form that says what happens to the caller's pointer.
     * */
    bool SetParent(Node& newParent);

    /**
     * Unhooks this node from its parent and hands ownership to the caller. Null
     * if it was already a root, in which case nothing changed.
     *
     * For reparenting, prefer SetParent — this is for when you actually want the
     * subtree in hand, or want it gone.
     *
     * nodiscard because dropping the result destroys the whole subtree. That is
     * a legitimate way to delete a branch, but it should be written on purpose:
     *
     *     node->Detach();              // warns
     *     node->Detach().reset();      // deletes the subtree, deliberately
     * */
    [[nodiscard]] std::unique_ptr<Node> Detach();

    // --- matrices ---------------------------------------------------------

    /** Local transform: T * R * S. Scale first onto the vector, then rotate, then translate. */
    glm::mat4 LocalMatrix() const;

    /**
     * The CACHED world matrix — the cheap read, O(1).
     *
     * Valid only after UpdateWorldMatrices has run over this node this frame.
     * Change a transform after that and read this in the same frame and you see
     * the previous frame's value. A node outside any updated tree never gets one
     * at all.
     * */
    const glm::mat4& worldMatrix() const { return worldMatrix_; }

    /**
     * Recomposes THIS node's cached matrix onto its parent's cached one. The
     * single step of a traversal somebody else owns — World::Update runs it
     * interleaved with the node's behaviours, which is why the step is exposed
     * rather than buried inside the recursive version below.
     *
     * Assumes the parent's cache was already refreshed this frame. Top-down
     * order is the whole reason the tree costs O(n) and not O(n * depth).
     * */
    void UpdateWorldMatrix();

    /**
     * UpdateWorldMatrix on this node and every descendant. Two callers: setting
     * up a tree outside the frame loop, and refreshing a subtree after something
     * moved its root mid-frame (which is what a late update does).
     * */
    void UpdateWorldMatrices();

    /**
     * The world matrix computed FRESH, walking up the parent chain — O(depth)
     * per call, always current. For use outside the frame loop, where the cache
     * has not been refreshed: right after building a tree, for instance. In the
     * frame's hot path, read worldMatrix().
     * */
    glm::mat4 ComputeWorldMatrix() const;

private:
    Node() = default;

    glm::quat rotation_{1.0f, 0.0f, 0.0f, 0.0f};

    /**
     * The last Euler angles set by hand, in degrees and unclamped — may hold
     * 500, may hold -720.5. Only trustworthy while eulerInSync_ is true.
     *
     * mutable because reading eulerAngles() on a stale cache derives and stores
     * the canonical angles. That is a change to the cache, not to the rotation:
     * the node's actual orientation is identical before and after, which is
     * exactly the case mutable exists for.
     * */
    mutable glm::vec3 euler_{0.0f, 0.0f, 0.0f};
    mutable bool eulerInSync_ = true;

    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;

    glm::mat4 worldMatrix_{1.0f};
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_NODE_H
