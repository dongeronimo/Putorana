#ifndef PUTORANA_GRAPHICS_HELLOWORLD_H
#define PUTORANA_GRAPHICS_HELLOWORLD_H

#include "ArCamera.h"
#include "CameraFeed.h"
#include "FinalPass.h"
#include "PerspectiveCamera.h"
#include "FlatColorMaterial.h"
#include "MeshPass.h"
#include "World.h"

#include <memory>
#include <string>

namespace putorana::graphics {

/**
 * The first world: one cube from a .glb, one camera, spinning.
 *
 * It exists to exercise the whole chain end to end — asset read, glTF parse,
 * mesh upload, node graph, material, pipeline cache, instanced draw, composite —
 * with the smallest content that can show each of those is wrong. A cube is a
 * good test object precisely because it is unforgiving: wrong winding hides the
 * faces you should see and shows the ones you should not, wrong depth order
 * makes the far faces draw over the near ones, and wrong normals turn it into a
 * flat hexagon.
 *
 * Structure, as asked for: ROOT, with the loaded cube under it and a second node
 * carrying the camera at (2,3,5) looking at the origin.
 * */
class HelloWorld : public World {
public:
    explicit HelloWorld(Device& device) : World(device) {}

    bool CreateRenderPasses(const Swapchain& swapchain, std::string& error) override;
    bool CreateWorld(std::string& error) override;
    void Update(float deltaSeconds) override;
    void Render(const FrameContext& frame) override;

private:
    std::unique_ptr<MeshPass> meshPass_;
    /**
     * The camera's textures, uploaded before the mesh pass and composited by the
     * final pass. Not a pass itself — it opens no rendering scope.
     *
     * Null when it could not be built. The world then draws the cube on the
     * cornflower clear exactly as it did before, which is also what happens on a
     * device with no ARCore and for the first frames while the camera stack
     * comes up.
     * */
    std::unique_ptr<CameraFeed> cameraFeed_;
    std::unique_ptr<FinalPass> finalPass_;

    /**
     * The node the cube's geometry hangs off, so Update can spin it. Non-owning:
     * the tree owns it, and it lives as long as this world does.
     * */
    Node* spinner_ = nullptr;
    float spinDegrees_ = 0.0f;

    /**
     * The camera node, and the AR camera hanging off it. Both non-owning: the
     * tree owns the node and the node owns the camera.
     *
     * arCamera_ is null when there is no AR session, which is also when the node
     * carries a PerspectiveCamera instead. It is kept as the derived type
     * because feeding it a frame is not something the Camera interface does — a
     * projection is all a render pass needs, and SetFrame is upkeep the world
     * owns.
     * */
    Node* cameraNode_ = nullptr;
    ArCamera* arCamera_ = nullptr;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_HELLOWORLD_H
