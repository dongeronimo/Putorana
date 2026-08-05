#include "Node.h"
#include "../behavours/BaseBehaviour.h"
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * Euler degrees -> quaternion, in the engine's convention: Z, then X, then Y,
 * about WORLD axes, so R = Ry * Rx * Rz. Unity's order.
 *
 * Written as an explicit product of three axis rotations rather than through a
 * library helper, because every library spells the order differently and half of
 * them mean intrinsic axes by it. Three lines that say what they do beat a
 * three-letter string nobody can decode later.
 * */
glm::quat EulerDegreesToQuat(const glm::vec3& degrees) {
    const glm::vec3 radians = glm::radians(degrees);
    return glm::angleAxis(radians.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
           glm::angleAxis(radians.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
           glm::angleAxis(radians.z, glm::vec3(0.0f, 0.0f, 1.0f));
}

/**
 * The inverse: canonical Euler degrees out of a unit quaternion, same
 * convention. x lands in [-90,90], y and z in [-180,180].
 *
 * The `2 * (...)` expressions are elements of the rotation matrix the quaternion
 * is equivalent to, from which the angles fall out by asin and atan2.
 * */
glm::vec3 QuatToEulerDegrees(const glm::quat& q) {
    const float x = q.x;
    const float y = q.y;
    const float z = q.z;
    const float w = q.w;

    // Sine of the X angle. |sinX| close to 1 is gimbal lock: aiming straight up
    // or straight down, the Y and Z turns happen about the same axis and stop
    // being tellable apart.
    const float sinX = 2.0f * (w * x - y * z);

    glm::vec3 degrees;
    if (std::abs(sinX) < 0.9999999f) {
        degrees.x = glm::degrees(std::asin(sinX));
        degrees.y = glm::degrees(
                std::atan2(2.0f * (x * z + w * y), 1.0f - 2.0f * (x * x + y * y)));
        degrees.z = glm::degrees(
                std::atan2(2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z)));
    } else {
        // Locked. Pin z to 0 by convention and give the whole remaining turn to
        // y — the same choice Unity and three.js make.
        degrees.x = sinX > 0.0f ? 90.0f : -90.0f;
        degrees.y = glm::degrees(
                std::atan2(2.0f * (w * y - x * z), 1.0f - 2.0f * (y * y + z * z)));
        degrees.z = 0.0f;
    }
    return degrees;
}

/**
 * The rotation part of a transform, with scale divided out, so quat_cast reads
 * an orthonormal basis. Feeding it a scaled basis produces a quaternion that is
 * not a rotation at all.
 * */
glm::mat3 NormalizedBasis(const glm::mat4& m) {
    glm::mat3 basis(m);
    for (int column = 0; column < 3; ++column) {
        const float length = glm::length(basis[column]);
        if (length > 0.0f) {
            basis[column] /= length;
        }
    }
    return basis;
}

} // namespace

std::unique_ptr<Node> Node::Create(std::string name) {
    auto node = std::unique_ptr<Node>(new Node());
    node->name = std::move(name);
    return node;
}

// --- rotation -------------------------------------------------------------

void Node::SetRotation(const glm::quat& q) {
    rotation_ = glm::normalize(q);
    // The raw angles are now a lie: a quaternion cannot say whether it got here
    // by turning 30 degrees or 390.
    eulerInSync_ = false;
}

glm::vec3 Node::eulerAngles() const {
    if (!eulerInSync_) {
        euler_ = QuatToEulerDegrees(rotation_);
        eulerInSync_ = true;
    }
    return euler_;
}

void Node::SetEulerAngles(const glm::vec3& degrees) {
    // Keep the numbers exactly as given so reading them back is lossless, and
    // sync the quaternion, which is what the rest of the engine consumes.
    euler_ = degrees;
    rotation_ = EulerDegreesToQuat(degrees);
    eulerInSync_ = true;
}

void Node::LookAt(const glm::vec3& target, const glm::vec3& up) {
    const glm::mat4 world = ComputeWorldMatrix();
    const glm::vec3 eye(world[3]);

    // +Z of the node points AWAY from what it looks at, since -Z is forward.
    glm::vec3 back = eye - target;
    const float distance = glm::length(back);
    if (distance <= 0.0f) {
        // Standing on the target. There is no direction to face, and every
        // formula below divides by this — leave the rotation alone.
        return;
    }
    back /= distance;

    // An `up` parallel to the aim degenerates the basis into zeros and the
    // quaternion into NaN, which then spreads silently through every matrix the
    // node touches. Aiming a camera straight down is not an exotic case, so
    // substitute an axis that cannot be parallel instead of producing garbage.
    glm::vec3 chosenUp = up;
    const float upLength = glm::length(chosenUp);
    if (upLength <= 0.0f || std::abs(glm::dot(chosenUp / upLength, back)) > 0.9999f) {
        chosenUp = std::abs(back.y) > 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                           : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    const glm::vec3 right = glm::normalize(glm::cross(chosenUp, back));
    const glm::vec3 trueUp = glm::cross(back, right);
    // Columns, in glm's column-major layout: the node's local X, Y and Z axes
    // expressed in world space, which is exactly a rotation matrix.
    glm::quat worldRotation = glm::quat_cast(glm::mat3(right, trueUp, back));

    if (parent_ != nullptr) {
        // local = inverse(parent's world rotation) * desired world rotation
        const glm::quat parentRotation =
                glm::quat_cast(NormalizedBasis(parent_->ComputeWorldMatrix()));
        worldRotation = glm::inverse(parentRotation) * worldRotation;
    }
    SetRotation(worldRotation);
}

void Node::CopyLocalFrom(const Node& src) {
    position = src.position;
    scale = src.scale;
    // Straight into the members, deliberately not through SetRotation /
    // SetEulerAngles: going through the setters would either flatten the raw
    // angles or rebuild the quaternion from them. Copying all three pieces plus
    // the flag reproduces the source's state exactly, whichever of the two it
    // was last edited through.
    rotation_ = src.rotation_;
    euler_ = src.euler_;
    eulerInSync_ = src.eulerInSync_;
}

// --- hierarchy ------------------------------------------------------------

Node* Node::AddChild(std::unique_ptr<Node> child) {
    if (child == nullptr) {
        return nullptr;
    }

    // Walk up from here. If the incoming child turns up as an ancestor — or is
    // this node — attaching it would make a subtree that owns itself: nothing
    // would ever destroy it, and the destructor would recurse forever if
    // anything tried.
    for (const Node* ancestor = this; ancestor != nullptr; ancestor = ancestor->parent_) {
        if (ancestor == child.get()) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "AddChild('%s' -> '%s') refused: that would make a cycle. The "
                                "subtree has been destroyed.",
                                child->name.c_str(), name.c_str());
            return nullptr;
        }
    }

    Node* raw = child.get();
    raw->parent_ = this;
    children_.push_back(std::move(child));
    return raw;
}

bool Node::SetParent(Node& newParent) {
    if (parent_ == &newParent) {
        return true;
    }
    if (parent_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "SetParent('%s' -> '%s') refused: '%s' is still a root, so its owner "
                            "holds the unique_ptr. Use newParent.AddChild(std::move(ptr)).",
                            name.c_str(), newParent.name.c_str(), name.c_str());
        return false;
    }

    // Checked here rather than left to AddChild, because by the time AddChild
    // could refuse, this node is already detached — a failed reparent would tear
    // the subtree out of the scene and then delete it. Refusing before anything
    // moves leaves the tree exactly as it was.
    for (const Node* ancestor = &newParent; ancestor != nullptr; ancestor = ancestor->parent_) {
        if (ancestor == this) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "SetParent('%s' -> '%s') refused: that would make a cycle",
                                name.c_str(), newParent.name.c_str());
            return false;
        }
    }

    newParent.AddChild(Detach());
    return true;
}

std::unique_ptr<Node> Node::Detach() {
    if (parent_ == nullptr) {
        return nullptr;
    }

    std::vector<std::unique_ptr<Node>>& siblings = parent_->children_;
    const auto found = std::find_if(siblings.begin(), siblings.end(),
                                    [this](const std::unique_ptr<Node>& sibling) {
                                        return sibling.get() == this;
                                    });
    if (found == siblings.end()) {
        // parent_ and the parent's children list are the two ends of one
        // relationship and are only ever written together, so this cannot
        // happen. Say so rather than erasing end().
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "Detach('%s'): parent '%s' does not list it as a child",
                            name.c_str(), parent_->name.c_str());
        parent_ = nullptr;
        return nullptr;
    }

    std::unique_ptr<Node> owned = std::move(*found);
    siblings.erase(found);
    parent_ = nullptr;
    return owned;
}

// --- matrices -------------------------------------------------------------

glm::mat4 Node::LocalMatrix() const {
    glm::mat4 m = glm::mat4_cast(rotation_);
    // Scaling the basis columns is a right-multiply by a scale matrix, so the
    // result is T * R * S: a vector is scaled first, then rotated, then moved.
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

void Node::UpdateWorldMatrix() {
    worldMatrix_ = parent_ != nullptr ? parent_->worldMatrix_ * LocalMatrix() : LocalMatrix();
}

void Node::UpdateWorldMatrices() {
    UpdateWorldMatrix();
    for (const std::unique_ptr<Node>& child : children_) {
        child->UpdateWorldMatrices();
    }
}

glm::mat4 Node::ComputeWorldMatrix() const {
    const glm::mat4 local = LocalMatrix();
    return parent_ != nullptr ? parent_->ComputeWorldMatrix() * local : local;
}

    void Node::runBehavioursStart() {
        for(auto b:behaviours) {
            if(!b->alreadyStarted()) {
                b->start();
            }
            if(b->hasLateUpdate() && !anyBehaviourHasLateUpdate) {
                anyBehaviourHasLateUpdate = true;
            }
        }
    }

    void Node::runBehavioursUpdate(float dt) {
        for(auto b:behaviours) {
            b->update(dt);
        }
    }

    void Node::runBehavioursLateUpdate(float dt) {
        for(auto b:behaviours) {
            b->runLateUpdate(dt);
        }
    }

    void Node::runBehavioursDispose() {
    for(auto b:behaviours) {
        b->dispose();
    }

    }

} // namespace putorana::graphics
