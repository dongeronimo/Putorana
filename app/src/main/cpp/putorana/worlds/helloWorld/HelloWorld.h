#ifndef ARRECONSTRUCTOR_HELLOWORLD_H
#define ARRECONSTRUCTOR_HELLOWORLD_H
#include "../../graphics/World.h"
#include "../../graphics/Device.h"
#include "../../graphics/Swapchain.h"
#include "../../graphics/MeshPass.h"
#include "../../graphics/FinalPass.h"
#include "../../behavours/BaseBehaviour.h"
#include <memory>
#include <vector>
namespace putorana::graphics {
    class MeshPass;
    class Material;
}
namespace putorana::worlds::hello {
    /**
     * The hello world World.
     * It shows a spinning cube over a plane. It tests behaviours, node tree,
     * loading of gltfs and basic render passes and rendering.
     * */
    class HelloWorld : public putorana::graphics::World{
    private:
        std::unique_ptr<graphics::MeshPass>  meshPass;
        std::unique_ptr<graphics::FinalPass> finalPass;
        /**
         * Every behaviour in this world, owned here.
         *
         * A Node holds its behaviours as RAW pointers and owns none of them, so
         * somebody has to, and the world is the only thing whose life covers the
         * whole tree. One behaviour per node it is attached to: they carry an
         * owner pointer, so sharing one between two nodes would make it lie
         * about which node it belongs to.
         *
         * Destroyed before ROOT is, since members of the derived class go before
         * the World base subobject. That leaves the nodes holding dangling
         * pointers for the length of a destructor, which is harmless only
         * because ~Node does not touch them. Anything that ever makes ~Node run
         * dispose() has to move this ownership into Node first.
         * */
        std::vector<std::unique_ptr<putorana::behaviours::BaseBehaviour>> behaviours;
        graphics::Material* CreateOrangeMaterial(std::string &error);
        graphics::Material* CreateGreenMaterial(std::string &error);
        graphics::Node* CreateCube(std::string& error);
        graphics::Node* CreatePlane(std::string& error);
    public:
        explicit HelloWorld(putorana::graphics::Device& device):World(device){}
        bool CreateRenderPasses(const putorana::graphics::Swapchain& swapchain,
                                std::string& error) override;
        bool CreateWorld(std::string& error) override;
        void Update(float deltaSeconds) override;
        void Render(const putorana::graphics::FrameContext& frame) override;
    };
}

#endif //ARRECONSTRUCTOR_HELLOWORLD_H
