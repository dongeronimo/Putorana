#include "HelloWorld.h"

#include "Device.h"
#include "GltfLoader.h"
#include "Swapchain.h"

#include <android/log.h>

#include <utility>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";
constexpr const char* kCubeAsset = "unitary_cube.glb";
constexpr const char* kMaterialName = "FlatOrange";

/** Slow enough to watch a face turn away, fast enough to be obviously alive. */
constexpr float kSpinDegreesPerSecond = 45.0f;

} // namespace

bool HelloWorld::CreateRenderPasses(const Swapchain& swapchain, std::string& error) {
    // The mesh pass draws into its OWN target, in the swapchain's format so the
    // composite is a straight copy. The day tone mapping matters this becomes a
    // float format and only the final pass has to know.
    meshPass_ = MeshPass::Create(device(), swapchain.format(), error);
    if (meshPass_ == nullptr) {
        return false;
    }
    finalPass_ = FinalPass::Create(device(), swapchain.format(), error);
    return finalPass_ != nullptr;
}

bool HelloWorld::CreateWorld(std::string& error) {
    // The material first: the loader leaves every renderable without one, and
    // assigning them below needs it to already exist.
    auto material = FlatColorMaterial::Create(device(), glm::vec4(0.95f, 0.55f, 0.15f, 1.0f),
                                              error);
    if (material == nullptr) {
        return false;
    }
    Material* flat = AddMaterial(kMaterialName, std::move(material));
    if (flat == nullptr) {
        error = "HelloWorld: could not register the material";
        return false;
    }

    GltfLoadResult loaded;
    if (!LoadGltf(*this, kCubeAsset, loaded, error)) {
        return false;
    }
    if (loaded.meshes.empty()) {
        error = std::string("HelloWorld: '") + kCubeAsset + "' produced no geometry";
        return false;
    }

    // The file's roots come back detached — this is where they join the world.
    for (std::unique_ptr<Node>& fileRoot : loaded.roots) {
        Node* attached = root().AddChild(std::move(fileRoot));
        if (spinner_ == nullptr) {
            spinner_ = attached;
        }
    }

    // Every renderable the file produced gets the one material there is. This is
    // the step that will read the node's "Material" custom property instead,
    // once the loader reads extras and there is more than one material to pick.
    for (Node* node : loaded.nodes) {
        if (node->renderable.has_value()) {
            node->renderable->material = flat;
        }
    }

    // The second node: the camera. A sibling of the cube rather than a child,
    // so spinning the cube does not spin the camera with it.
    Node* eye = root().AddChild(Node::Create("camera"));
    eye->camera.emplace();
    eye->position = glm::vec3(2.0f, 3.0f, 5.0f);
    // LookAt reads the node's world matrix, and it is fresh here because
    // ComputeWorldMatrix walks the parent chain rather than trusting the cache —
    // which has not been filled yet, since no frame has run.
    eye->LookAt(glm::vec3(0.0f, 0.0f, 0.0f));

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "HelloWorld ready: %zu meshes, camera at (2,3,5) aimed at the origin",
                        loaded.meshes.size());
    return true;
}

void HelloWorld::Update(float deltaSeconds) {
    if (spinner_ != nullptr) {
        // Accumulated in degrees and never wrapped, which is exactly what the
        // unbounded Euler interface is for: the angle keeps climbing past 360
        // and the quaternion it drives is perfectly well behaved.
        spinDegrees_ += kSpinDegreesPerSecond * deltaSeconds;
        // Tilted a little on X so the top face comes into view — a cube spun
        // only about Y shows the same silhouette all the way round.
        spinner_->SetEulerAngles(glm::vec3(20.0f, spinDegrees_, 0.0f));
    }

    // Last, and it must be: the base closes every world matrix, so anything
    // moved after this call would land a frame late.
    World::Update(deltaSeconds);
}

void HelloWorld::Render(const FrameContext& frame) {
    meshPass_->Render(frame, root());

    const Image* color = meshPass_->colorTarget();
    if (color == nullptr) {
        // The mesh pass gave up on its targets — a window with no area, most
        // likely. Nothing to composite.
        return;
    }
    finalPass_->Render(frame, *color);
}

} // namespace putorana::graphics
