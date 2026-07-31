#ifndef PUTORANA_GRAPHICS_HELLOWORLD_H
#define PUTORANA_GRAPHICS_HELLOWORLD_H

#include "FinalPass.h"
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
    std::unique_ptr<FinalPass> finalPass_;

    /**
     * The node the cube's geometry hangs off, so Update can spin it. Non-owning:
     * the tree owns it, and it lives as long as this world does.
     * */
    Node* spinner_ = nullptr;
    float spinDegrees_ = 0.0f;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_HELLOWORLD_H
