#include "World.h"

#include "Device.h"

#include <android/log.h>

#include <algorithm>
#include <utility>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

Node* FindInSubtree(Node& node, const std::string& name) {
    if (node.name == name) {
        return &node;
    }
    for (const std::unique_ptr<Node>& child : node.children()) {
        if (Node* found = FindInSubtree(*child, name)) {
            return found;
        }
    }
    return nullptr;
}

bool IsQueued(const std::vector<Node*>& queue, const Node* node) {
    return std::find(queue.begin(), queue.end(), node) != queue.end();
}

} // namespace

World::World(Device& device) : device_(device), root_(Node::Create("ROOT")) {}

// Defaulted, but out of line so the vtable and RTTI are anchored here rather
// than in every TU that includes the header.
//
// The order everything goes away in is decided by the declaration order in
// World.h and nothing else: root_ is declared last, so it is destroyed first,
// so the scene is gone before the meshes and materials it pointed at.
World::~World() = default;

Node* World::FindNode(const std::string& name) const {
    return FindInSubtree(*root_, name);
}

Mesh* World::AddMesh(std::unique_ptr<Mesh> mesh) {
    if (mesh == nullptr) {
        return nullptr;
    }
    Mesh* raw = mesh.get();
    meshes_.push_back(std::move(mesh));
    return raw;
}

Material* World::AddMaterial(std::string name, std::unique_ptr<Material> material) {
    if (material == nullptr) {
        return nullptr;
    }
    // Refusing beats replacing: a silent replace destroys the old material while
    // renderables are still pointing at it, and the symptom would surface far
    // from here as a use-after-free in a descriptor set.
    if (materials_.find(name) != materials_.end()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "AddMaterial('%s') refused: that name is already taken",
                            name.c_str());
        return nullptr;
    }
    Material* raw = material.get();
    materials_.emplace(std::move(name), std::move(material));
    return raw;
}

Material* World::FindMaterial(const std::string& name) const {
    const auto found = materials_.find(name);
    return found != materials_.end() ? found->second.get() : nullptr;
}

void World::DestroyNode(Node& node) {
    if (&node == root_.get()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "DestroyNode(ROOT) refused: the world owns its root");
        return;
    }
    if (IsQueued(pendingDestruction_, &node)) {
        return;
    }
    pendingDestruction_.push_back(&node);
}

void World::Update(float deltaSeconds) {
    std::vector<Node*> lateNodes = {};
    // Behaviours are driven from inside this traversal, interleaved with the
    // matrix update — see the contract in World.h. Until the Behaviour class
    // exists there is nothing for the delta to feed, and the traversal is
    // purely the matrix pass.
    Visit(root_.get(), deltaSeconds, lateNodes);

    for(auto node:lateNodes) {
        node->runBehavioursLateUpdate(deltaSeconds);
        node->UpdateWorldMatrices();
    }
    // Only now, with the traversal finished, is it safe to touch the tree: a
    // node erased mid-walk would invalidate the very vector being iterated.
    FlushPendingDestruction();
}

void World::Visit(Node* node, float dt, std::vector<Node*>& lateNodes) {
    node->runBehavioursStart();
    if(node->hasLateUpdate()) {
        lateNodes.push_back(node);
    }
    node->runBehavioursUpdate(dt);
    node->UpdateWorldMatrix();
    auto& children = node->children();
    for(auto& child:children) {
        Visit(child.get(), dt, lateNodes);
    }
}
void World::FlushPendingDestruction() {
    if (pendingDestruction_.empty()) {
        return;
    }

    // Drop anything already covered by a queued ancestor. Destroying the
    // ancestor frees the whole subtree, so the descendant's entry would be a
    // pointer to freed memory by the time its turn came — the JavaScript version
    // gets away with queueing both because a garbage collector does not care.
    for (size_t i = 0; i < pendingDestruction_.size();) {
        bool coveredByAncestor = false;
        for (const Node* ancestor = pendingDestruction_[i]->parent();
             ancestor != nullptr && !coveredByAncestor; ancestor = ancestor->parent()) {
            coveredByAncestor = IsQueued(pendingDestruction_, ancestor);
        }
        if (coveredByAncestor) {
            pendingDestruction_.erase(pendingDestruction_.begin() +
                                      static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    for (Node* node : pendingDestruction_) {
        // Behaviour dispose() belongs here, once behaviours exist: per-instance
        // state has to be released while the node is still whole.
        //
        // Detach hands back the owning pointer and reset destroys it, taking the
        // subtree with it. Meshes and materials are untouched — they are shared
        // and stay the world's.
        node->runBehavioursDispose();
        node->Detach().reset();
    }
    pendingDestruction_.clear();
}

} // namespace putorana::graphics
