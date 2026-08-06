#ifndef ARRECONSTRUCTOR_OPENCHISELWORLD_H
#define ARRECONSTRUCTOR_OPENCHISELWORLD_H

#include "graphics/ArCamera.h"
#include "graphics/CameraFeed.h"
#include "graphics/Device.h"
#include "graphics/FinalPass.h"
#include "graphics/MeshPass.h"
#include "graphics/PerspectiveCamera.h"
#include "graphics/SpaceChunk.h"
#include "graphics/Swapchain.h"
#include "graphics/World.h"

#include "recon/Reconstruction.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace putorana::worlds::openChisel {

/**
 * The OpenChisel reconstruction, drawn over the camera feed.
 *
 * This was the app's only world for a long time, and it was called HelloWorld
 * for as long: it began as one cube from a .glb, spinning, to exercise the whole
 * chain end to end: asset read, glTF parse, mesh upload, node graph, material,
 * pipeline cache, instanced draw, composite. The cube is gone; it did that job,
 * and then a second one as the marker that proved the unprojection maths. What
 * it proved the reconstruction now proves continuously and much harder. The name
 * went with the cube, to putorana::worlds::hello, which is a hello world again.
 *
 * Structure: ROOT, with one node carrying the camera, and one node per chunk of
 * reconstructed space arriving beneath it as the sensor sees more of the room.
 * The chunk nodes are a CACHE (see chunkNodes) because this world dies with
 * the Device on every trip to background AND on every switch to another world,
 * while the reconstruction does not.
 * */
class OpenChiselWorld : public putorana::graphics::World {
public:
    explicit OpenChiselWorld(putorana::graphics::Device& device) : World(device) {}

    bool CreateRenderPasses(const putorana::graphics::Swapchain& swapchain,
                            std::string& error) override;
    bool CreateWorld(std::string& error) override;
    void Update(float deltaSeconds) override;
    void Render(const putorana::graphics::FrameContext& frame) override;

private:
    /**
     * Fuse this frame's depth, remesh a bounded number of chunks, and bring the
     * node tree into line with the result.
     *
     * Runs in Update rather than Render because it CREATES nodes, and
     * World::Update closes every world matrix: a node added after that would
     * have no transform for a frame. The GPU uploads cannot happen here though:
     * Update runs before FrameRing::BeginFrame, so the region about to be
     * written may still be in flight. They happen in Render instead.
     * */
    void UpdateReconstruction(const ar::CameraFrame& frame);

    /** Uploads whatever UpdateReconstruction marked. Render only: needs the slot. */
    void UploadDirtyChunks(uint32_t frameIndex);

    std::unique_ptr<graphics::MeshPass> meshPass;
    /**
     * The camera's textures, uploaded before the mesh pass and composited by the
     * final pass. Not a pass itself: it opens no rendering scope.
     *
     * Null when it could not be built. The world then draws the chunks on the
     * cornflower clear, which is also what happens on a device with no ARCore
     * and for the first frames while the camera stack comes up.
     * */
    std::unique_ptr<graphics::CameraFeed> cameraFeed;
    std::unique_ptr<graphics::FinalPass> finalPass;

    // --- the reconstruction ------------------------------------------------

    /**
     * One node per chunk of reconstructed space, found by its grid coordinates.
     *
     * A CACHE, not the source of truth. This world belongs to a Device and a
     * Device dies every time the app is backgrounded, so every entry here is
     * destroyed while recon::Reconstruction, at process scope, keeps the
     * voxels. Switching worlds destroys them too, and for the same reason: the
     * world is what the switch replaces. CreateWorld therefore asks the
     * reconstruction to re-report everything rather than assuming an empty scene
     * means an empty world.
     * */
    struct ChunkNode {
        graphics::Node* node = nullptr;
        graphics::Mesh* mesh = nullptr;

        /**
         * How many more frames this chunk's geometry has to be re-uploaded.
         *
         * Not a bool, and this is the subtlety that would otherwise show up as
         * chunks flickering at half the frame rate. Mesh::Update writes ONE
         * region (indexCounts_ is per region) so a chunk uploaded once has
         * data for one frame index and an empty region for the other. It has to
         * be written on kFramesInFlight consecutive frames, one region each,
         * because the region not in use this frame may still be being read by
         * the GPU.
         * */
        uint32_t uploadsRemaining = 0;

        /**
         * What the last ChunkUpdate reported. Carried here because WriteChunk
         * fills only as much of the scratch buffer as the chunk actually has,
         * and uploading the buffer's full size instead would hand the mesh
         * thousands of uninitialised vertices and an index count to match.
         * */
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
    };
    std::unordered_map<graphics::SpaceChunk, ChunkNode, graphics::SpaceChunkHash> chunkNodes;

    /** Non-owning; the world owns it. Shared by every chunk node. */
    graphics::Material* chunkMaterial = nullptr;

    /**
     * Where WriteChunk interleaves before Mesh::Update copies into the mapped
     * buffer.
     *
     * Two copies rather than one, and it was a deliberate reversal. Handing the
     * reconstruction a mapped pointer to write straight into would skip this,
     * but Mesh narrows indices to uint16 whenever the capacity allows, and a
     * caller writing raw uint32 into that buffer would corrupt it. Halving index
     * bandwidth on every chunk, every frame, is worth more than one pass over
     * ~640 KB of CPU memory.
     * */
    std::vector<recon::Vertex> vertexScratch;
    std::vector<uint32_t> indexScratch;

    /** Reused across frames so the per-frame path allocates nothing. */
    std::vector<recon::ChunkUpdate> chunkChanged;
    std::vector<recon::ChunkKey> chunkRemoved;

    /**
     * Vertex counts seen so far, bucketed, logged once. The mesh capacity below
     * is a guess and this is what replaces it with a measurement; see the note
     * on MeshStorage::Mutable in Mesh.h: capacity is fixed at creation, so
     * choosing it badly either truncates dense chunks or wastes most of the
     * largest allocation in the app.
     * */
    std::array<uint32_t, 8> chunkVertexHistogram{};
    uint32_t chunkMeshCount = 0;
    uint32_t chunkOverflowCount = 0;
    bool loggedChunkHistogram = false;

    /**
     * Totals behind the deduplication ratio, indices / vertices.
     *
     * Summed across chunks rather than averaged per chunk, so a few large
     * chunks count for what they are instead of being one sample each.
     * */
    uint64_t totalChunkVertices = 0;
    uint64_t totalChunkIndices = 0;

    /**
     * Node count at the last growth report.
     *
     * The app has now died twice with the graphics allocator out of memory once
     * a room's worth of chunks accumulated, and both times nothing was watching
     * the number that explains it. Chunk meshes are never freed -- a chunk that
     * empties keeps its allocation for when it comes back -- so this only goes
     * up, and its SLOPE is what says whether a fixed capacity per chunk can work
     * at all or whether the meshes have to come from a pool.
     * */
    size_t lastReportedNodeCount = 0;

    /**
     * The camera node, and the AR camera hanging off it. Both non-owning: the
     * tree owns the node and the node owns the camera.
     *
     * arCamera is null when there is no AR session, which is also when the node
     * carries a PerspectiveCamera instead. It is kept as the derived type
     * because feeding it a frame is not something the Camera interface does: a
     * projection is all a render pass needs, and SetFrame is upkeep the world
     * owns.
     * */
    graphics::Node* cameraNode = nullptr;
    graphics::ArCamera* arCamera = nullptr;
};

} // namespace putorana::worlds::openChisel

#endif //ARRECONSTRUCTOR_OPENCHISELWORLD_H
