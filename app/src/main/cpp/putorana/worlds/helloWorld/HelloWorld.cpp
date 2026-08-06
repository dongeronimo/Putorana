#include "HelloWorld.h"
#include "../../graphics/MeshPass.h"
#include "../../graphics/FlatColorMaterial.h"
#include "graphics/GltfLoader.h"
#include "graphics/PerspectiveCamera.h"
#include "behavours/RotationBehaviour.h"

using namespace putorana::graphics;
namespace putorana::worlds::hello {
    /**
     * For now we'll use 2 render passes: the main pass for meshes and the final pass that
     * renders to the swap chain.
     * */
    bool HelloWorld::CreateRenderPasses(const Swapchain &swapchain, std::string &error) {
        meshPass = MeshPass::Create(device(), swapchain.format(), error);
        finalPass = FinalPass::Create(device(), swapchain.format(), error);
        return (meshPass && finalPass);
    }

    Material* HelloWorld::CreateOrangeMaterial(std::string &error) {
        auto mat = FlatColorMaterial::Create(device(), glm::vec4(0.95f, 0.55f, 0.15f, 1.0f),error);
        if (mat == nullptr)
            return nullptr;
        Material* flatOrange = AddMaterial("FlatOrange", std::move(mat));
        if (flatOrange == nullptr) {
            error = "HelloWorld: material name already taken";
            return nullptr;
        }
        return flatOrange;
    }

    Material* HelloWorld::CreateGreenMaterial(std::string &error) {
        auto mat = FlatColorMaterial::Create(device(), glm::vec4(0.33f, 0.66f, 0.33f, 1.0f),error);
        if (mat == nullptr)
            return nullptr;
        Material* flatGreen = AddMaterial("FlatGreen", std::move(mat));
        if (flatGreen == nullptr) {
            error = "HelloWorld: material name already taken";
            return nullptr;
        }
        return flatGreen;
    }

    graphics::Node *HelloWorld::CreateCube(std::string &error) {
        GltfLoadResult loaded;
        if (!LoadGltf(*this, "unitary_cube.glb", loaded, error))
            return nullptr;
        if (loaded.meshes.empty()) {
            error = "HelloWorld: 'unitary_cube.glb' produced no geometry";
            return nullptr;
        }
        Node* cube = nullptr;
        for (std::unique_ptr<Node>& fileRoot : loaded.roots) {
            Node* attached = root().AddChild(std::move(fileRoot));
            if (cube == nullptr) cube = attached;
        }
        auto mat = FindMaterial("FlatOrange");
        for (Node* node : loaded.nodes) {
            if (node->renderable.has_value()) node->renderable->material = mat;
        }
        return cube;
    }

    graphics::Node *HelloWorld::CreatePlane(std::string &error) {
        GltfLoadResult loaded;
        if (!LoadGltf(*this, "unitary_plane.glb", loaded, error))
            return nullptr;
        if (loaded.meshes.empty()) {
            error = "HelloWorld: 'unitary_plane.glb' produced no geometry";
            return nullptr;
        }
        auto planeRoot = Node::Create("Planes");
        //create the many plates. In the future we'll use some kind of prefab system, it' doesn't
        //exist yet.
        for(int x = -5; x<5; x++){
            for(int z = -5; z<5; z++){
                std::unique_ptr<Node> plane = Node::Create("plane");
                plane->position = glm::vec3(x,0,z);
                Renderable renderable(*loaded.meshes[0]);
                renderable.material = FindMaterial("FlatGreen");
                plane->renderable = renderable;
                planeRoot->AddChild(std::move(plane));
            }
        }
        return root().AddChild(std::move(planeRoot));
    }

    bool HelloWorld::CreateWorld(std::string &error) {
        // Create and store the materials
        Material* flatOrange = CreateOrangeMaterial(error);
        if(!flatOrange) return false;
        Material* flatGreen = CreateGreenMaterial(error);
        if(!flatGreen) return false;
        //create the cube
        Node* cube = CreateCube(error);
        if(!cube)return false;
        cube->position = glm::vec3(0.0f, 1.5f, 0.0f);
        //and spin it. 30 deg/s about Y: slow enough to watch a face turn away,
        //fast enough that a stopped frame loop is obvious.
        //
        //The node takes a raw pointer and owns nothing, so the unique_ptr has to
        //land in this world's `behaviours` before CreateWorld returns.
        auto spin = std::make_unique<putorana::behaviours::RotationBehaviour>(
                cube, glm::vec3(0.0f, 30.0f, 0.0f));
        cube->behaviours.push_back(spin.get());
        behaviours.push_back(std::move(spin));
        //create the plane
        Node* planes = CreatePlane(error);
        if(!planes)return false;
        //create the camera
        Node* eye = root().AddChild(Node::Create("camera"));
        eye->camera = PerspectiveCamera::Create();
        eye->position = glm::vec3(2.0f, 3.0f, 5.0f);
        eye->LookAt(glm::vec3(0.0f, 0.0f, 0.0f));  // lê a world matrix, e ela é fresca aqui
        return true;
    }

    void HelloWorld::Update(float deltaSeconds) {
        World::Update(deltaSeconds);
    }

    void HelloWorld::Render(const FrameContext &frame) {
        {
            GpuScope scope(frame, "mesh");
            meshPass->Render(frame, root(), false);
        }
        const Image* color = meshPass->colorTarget();
        if(color == nullptr){
            return;
        }
        {
            GpuScope scope(frame, "final");
            finalPass->Render(frame, *color, nullptr);
        }
    }

}
