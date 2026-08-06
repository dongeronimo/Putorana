#include "OpenChiselWorld.h"

#include "graphics/ChunkMaterial.h"
#include "graphics/Device.h"
#include "graphics/FrameRing.h"
#include "graphics/Swapchain.h"

#include <algorithm>

#include <android/log.h>

#include <cstddef>
#include <utility>

using namespace putorana::graphics;

namespace putorana::worlds::openChisel {

namespace {

constexpr const char* kLogTag = "ARReconstructor";
constexpr const char* kChunkMaterialName = "ReconChunk";

// --- the seam between recon and graphics ---
//
// recon::Vertex and graphics::ReconstructedVertex describe the same 28 bytes and
// are declared twice on purpose: graphics/Mesh.h includes volk.h and
// putorana::recon may not, so neither namespace can use the other's struct. This
// is the one translation unit that sees both, which makes it the only place the
// duplication can be checked.
//
// It was checked nowhere at all until colour arrived, and the header comment
// claiming otherwise was aspirational. Two members that were both "position then
// normal" made a drift unlikely; three, one of them a packed byte quad, do not.
// A mismatched offset here is colour read from the normal's bytes, silently.
static_assert(sizeof(recon::Vertex) == sizeof(ReconstructedVertex),
              "recon::Vertex and graphics::ReconstructedVertex must be the same size");
static_assert(alignof(recon::Vertex) == alignof(ReconstructedVertex),
              "recon::Vertex and graphics::ReconstructedVertex must have the same alignment");
static_assert(offsetof(recon::Vertex, position) == offsetof(ReconstructedVertex, position),
              "position must sit at the same offset in both vertex structs");
static_assert(offsetof(recon::Vertex, normal) == offsetof(ReconstructedVertex, normal),
              "normal must sit at the same offset in both vertex structs");
static_assert(offsetof(recon::Vertex, color) == offsetof(ReconstructedVertex, color),
              "colour must sit at the same offset in both vertex structs");

/**
 * Chunks remeshed per frame. Integration marks the whole 3x3x3 neighbourhood of
 * every chunk it touches, so a sweep of a room dirties hundreds at once and
 * marching cubes on all of them inside one frame is a dropped frame. What is not
 * done stays marked.
 * */
constexpr uint32_t kRemeshBudgetPerFrame = 8;

/**
 * Vertices a chunk mesh RESERVES. Measured, and the distinction between reserved
 * and used is the whole point of this comment.
 *
 * MeshStorage::Mutable fixes capacity at creation and reserves
 * kFramesInFlight copies of it, so a chunk costs the same whether it holds forty
 * vertices or four thousand. At 4096 that was 240 KiB per chunk, and a room's
 * worth of chunks took the process out of memory -- the symptom being ARCore's
 * camera ImageReader failing to allocate, because by then nothing could.
 *
 * The histogram below measures what is USED, and after deduplication every one
 * of 203 remeshes came in under 512 vertices. Deduplication cut usage 4.48x and
 * cut reservation by nothing at all, which is exactly how "there is plenty of
 * headroom" and "we are out of memory" were true at the same time.
 *
 * 1024 leaves twice the measured maximum.
 * */
constexpr uint32_t kChunkVertexCapacity = 1024;

/**
 * FIVE times the vertex capacity, and this ratio inverted when deduplication
 * landed. It is worth stating why, because both the old value and the obvious
 * correction are wrong now.
 *
 * Upstream emitted a triangle soup: three fresh vertices per triangle, so
 * indexCount == vertexCount exactly, and 3x was three times too much. Sharing
 * vertices does not change the number of indices -- still three per triangle --
 * it only shrinks the vertex count, so the ratio between them became the
 * deduplication ratio itself. That measured 4.48x on a real scan.
 *
 * So a chunk needs roughly 4.5 indices per vertex now, and sizing them equal
 * (which was correct for a soup) would make every chunk overflow and be dropped.
 * */
constexpr uint32_t kChunkIndexCapacity = kChunkVertexCapacity * 5;

/**
 * How far to trust the camera colour the reconstruction puts on each vertex.
 *
 * 1 is the point of all this. **Set it to 0 when the reconstruction looks wrong**
 * and you need to know whether the fault is the geometry or the colour on it.
 *
 * That is not a general-purpose brightness knob, it is a diagnostic, and the
 * reason it earns a constant here rather than living inside ChunkMaterial is that
 * camera colour is drawn nearly unlit -- it already contains the room's real
 * lighting -- and unlit shading hides exactly what the hardcoded directional
 * light was chosen to expose. A surface that is noisy, inside out, or facing the
 * wrong way is obvious under that light and invisible under a photograph of
 * itself. Zero here puts the light back.
 * */
constexpr float kChunkColorMix = 1.0f;

} // namespace

bool OpenChiselWorld::CreateRenderPasses(const Swapchain& swapchain, std::string& error) {
    // The mesh pass draws into its OWN target, in the swapchain's format so the
    // composite is a straight copy. The day tone mapping matters this becomes a
    // float format and only the final pass has to know.
    meshPass = MeshPass::Create(device(), swapchain.format(), error);
    if (meshPass == nullptr) {
        return false;
    }

    // Not a pass: it draws nothing. It only owns the camera's two textures, for
    // the final pass to composite. Its failure is not the world's: no background
    // still leaves the reconstruction drawn on the clear colour, which is worth
    // having. So the error is logged and swallowed here rather than returned.
    std::string cameraError;
    cameraFeed = CameraFeed::Create(device(), cameraError);
    if (cameraFeed == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "no camera background: %s",
                            cameraError.c_str());
    }

    finalPass = FinalPass::Create(device(), swapchain.format(), error);
    return finalPass != nullptr;
}

bool OpenChiselWorld::CreateWorld(std::string& error) {
    // No cube any more.
    //
    // It was scaffolding and it did its job twice: first as the thing that
    // proved geometry was arriving with the right winding, normals and depth
    // order, then as the marker that proved the unprojection maths: the frozen
    // marker staying nailed to a wall while the phone moved is what ruled out
    // both axis flips, the quaternion order and the choice of pose. The
    // reconstruction now tests all of that continuously and far harder, so a
    // spinning cube in the middle of it is just something in the way.

    // The camera is the only node this world builds. Everything else in the
    // scene arrives from the reconstruction, one node per chunk.
    Node* eye = root().AddChild(Node::Create("camera"));
    cameraNode = eye;

    // Which KIND of camera is the one decision this world makes about AR. With a
    // session, ARCore owns both the projection and the pose, and the node's
    // transform becomes an output rather than something set here.
    if (ar::Subsystem* subsystem = ar::subsystem_holder::Get()) {
        // The reconstruction, and the material its chunks are drawn with.
        //
        // reconstruction_holder, not a member: this world dies with the Device
        // every time the app is backgrounded and again on every switch to
        // another world, and the reconstruction must survive both. Create is a
        // no-op when one already exists, which is exactly what coming back looks
        // like either way.
        auto material = ChunkMaterial::Create(
                device(), glm::vec4(0.45f, 0.72f, 0.95f, 1.0f), error);
        if (material == nullptr) {
            return false;
        }
        // The flat blue above is no longer what the reconstruction looks like.
        // It is the fallback, drawn only where a vertex has geometry but no
        // camera colour behind it yet, which is every vertex on the frame it
        // first appears and fewer of them every second after.
        material->SetColorMix(kChunkColorMix);
        // What encoding the mesh pass's attachment wants. CreateRenderPasses has
        // already run, so the format is settled by now, and it has to come from
        // the pass rather than be assumed, because the swapchain's _SRGB
        // preference has _UNORM fallbacks behind it.
        material->SetSrgbTarget(IsSrgbFormat(meshPass->colorFormat()));
        chunkMaterial = AddMaterial(kChunkMaterialName, std::move(material));

        recon::Config reconConfig;
        if (recon::reconstruction_holder::Create(reconConfig, error) == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "reconstruction: %s", error.c_str());
        } else if (recon::Reconstruction* reconstruction =
                           recon::reconstruction_holder::Get()) {
            // Everything it already has, as if it had just changed. On a first
            // run this is empty; after a trip to background or a round trip
            // through another world it is the whole map, because every node
            // holding it was destroyed with this world.
            reconstruction->MarkAllDirty();
        }

        auto camera = ArCamera::Create();
        // ARCore is the one that builds the projection, so it is the one that
        // needs the clip planes. Setting them on the camera alone would change
        // nothing at all.
        subsystem->SetClipPlanes(camera->nearPlane, camera->farPlane);
        arCamera = camera.get();
        eye->camera = std::move(camera);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "OpenChiselWorld ready: AR camera, reconstruction on");
    } else {
        // No ARCore means no depth, so nothing will ever be reconstructed and
        // this scene stays empty: the camera feed is gone too, so the screen is
        // the clear colour and nothing else. That is the honest picture of what
        // this world can do on such a device; the hello world is the one with
        // something to look at there.
        eye->camera = PerspectiveCamera::Create();
        eye->position = glm::vec3(2.0f, 3.0f, 5.0f);
        // LookAt reads the node's world matrix, and it is fresh here because
        // ComputeWorldMatrix walks the parent chain rather than trusting the
        // cache, which has not been filled yet, since no frame has run.
        eye->LookAt(glm::vec3(0.0f, 0.0f, 0.0f));
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "OpenChiselWorld ready: no ARCore, so nothing to reconstruct and "
                            "nothing to draw");
    }
    return true;
}

void OpenChiselWorld::Update(float deltaSeconds) {
    // The AR camera first, because everything else this frame is measured
    // against where the device is. Frame.cpp already ticked the session before
    // calling this, so the frame here is the current one.
    //
    // Under the lock, for the same reason the upload is: Pause arrives on the UI
    // thread and can invalidate the frame mid-read.
    if (arCamera != nullptr && cameraNode != nullptr) {
        if (const ar::Subsystem* subsystem = ar::subsystem_holder::Get()) {
            const ar::Subsystem::FrameGuard guard = subsystem->AcquireFrame();
            if (const ar::CameraFrame* arFrame = guard.get()) {
                arCamera->SetFrame(*arFrame, *cameraNode);

                // Under the same guard, and it has to be: the depth pointer
                // belongs to an ArImage the next Update releases, and the pose
                // has to be this frame's or the geometry lands where the phone
                // used to be.
                UpdateReconstruction(*arFrame);
            }
        }
    }

    // Last, and it must be: the base closes every world matrix, so anything
    // moved after this call would land a frame late. That is also why the
    // reconstruction runs above rather than in Render: it creates nodes.
    World::Update(deltaSeconds);
}

void OpenChiselWorld::UpdateReconstruction(const ar::CameraFrame& frame) {
    recon::Reconstruction* reconstruction = recon::reconstruction_holder::Get();
    if (reconstruction == nullptr || chunkMaterial == nullptr || !frame.tracking) {
        return;
    }

    // The whole frame, not its pieces. Depth, colour and pose have to come from
    // one frame or the colour lands on geometry the phone has already moved away
    // from, and Reconstruction is where the choice of sensor pose over display
    // pose belongs. See Reconstruction::Integrate.
    reconstruction->Integrate(frame);
    reconstruction->Remesh(kRemeshBudgetPerFrame);
    reconstruction->CollectUpdates(chunkChanged, chunkRemoved);

    for (const recon::ChunkKey& key : chunkRemoved) {
        const auto found = chunkNodes.find(SpaceChunk{key.x, key.y, key.z});
        if (found == chunkNodes.end()) {
            continue;
        }
        // The node stays. Node has no RemoveChild, and it would be the wrong
        // thing anyway: a chunk that empties out is usually one the sensor has
        // not seen enough of yet, and it comes back. Clearing the renderable
        // hides it and leaves the mesh allocated for when it does.
        if (found->second.node != nullptr) {
            found->second.node->renderable.reset();
        }
        found->second.uploadsRemaining = 0;
    }

    for (const recon::ChunkUpdate& update : chunkChanged) {
        const SpaceChunk key{update.key.x, update.key.y, update.key.z};
        ChunkNode& entry = chunkNodes[key];

        if (entry.node == nullptr) {
            Node* node = root().AddChild(Node::Create("chunk"));
            node->position = update.origin;
            node->spaceChunk = key;
            entry.node = node;
        }

        if (entry.mesh == nullptr) {
            MeshDesc desc;
            desc.name = "chunk";
            desc.format = VertexFormat::Reconstructed;
            // Mutable: written again every time the chunk is remeshed, which
            // happens for as long as the sensor keeps seeing it.
            desc.storage = MeshStorage::Mutable;
            desc.vertexCapacity = kChunkVertexCapacity;
            desc.indexCapacity = kChunkIndexCapacity;

            std::string error;
            std::unique_ptr<Mesh> mesh =
                    Mesh::Create(device().allocator().handle(), desc, error);
            if (mesh == nullptr) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "chunk mesh: %s", error.c_str());
                continue;
            }
            entry.mesh = AddMesh(std::move(mesh));
        }

        // --- the measurement that settles kChunkVertexCapacity ---
        const size_t bucket = std::min<size_t>(chunkVertexHistogram.size() - 1,
                                               update.vertexCount / 512);
        ++chunkVertexHistogram[bucket];
        ++chunkMeshCount;

        // indices / vertices IS the deduplication ratio, and it needs no extra
        // plumbing to measure. Marching cubes emits three indices per triangle
        // whether or not vertices are shared, so the index count is 3F either
        // way; only the vertex count moves. A triangle soup therefore reads
        // exactly 1.00, and a fully shared manifold surface should read around
        // 6, since V is about F/2 there.
        //
        // Worth having as a number rather than a belief: if it comes back near
        // 1 the edge keys are not matching and the sharing is not happening,
        // and that would look identical on screen.
        totalChunkVertices += update.vertexCount;
        totalChunkIndices += update.indexCount;
        if (update.vertexCount > kChunkVertexCapacity ||
            update.indexCount > kChunkIndexCapacity) {
            ++chunkOverflowCount;
            // Skipped rather than truncated: half a chunk of geometry is a hole
            // with a torn edge, which reads as a reconstruction bug rather than
            // as the capacity problem it is.
            continue;
        }

        entry.vertexCount = update.vertexCount;
        entry.indexCount = update.indexCount;
        entry.uploadsRemaining = FrameRing::kFramesInFlight;
    }

    // Every 64 nodes. The failure this watches for is unbounded growth, so the
    // interesting quantity is how fast it climbs, not where it is.
    if (chunkNodes.size() >= lastReportedNodeCount + 64) {
        lastReportedNodeCount = chunkNodes.size();
        // What MeshStorage::Mutable actually reserves: kFramesInFlight regions
        // of (vertices + indices), whatever the chunk ends up using.
        const size_t bytesPerMesh =
                static_cast<size_t>(FrameRing::kFramesInFlight) *
                (kChunkVertexCapacity * sizeof(recon::Vertex) + kChunkIndexCapacity * 2);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE %zu chunk nodes reserving %zu MiB of mesh (%zu KiB each), "
                            "%u voxel chunks, %u awaiting remesh",
                            chunkNodes.size(), (chunkNodes.size() * bytesPerMesh) / (1024 * 1024),
                            bytesPerMesh / 1024, reconstruction->chunkCount(),
                            reconstruction->pendingRemeshCount());
    }

    if (!loggedChunkHistogram && chunkMeshCount >= 200) {
        loggedChunkHistogram = true;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE chunk vertex counts over %u remeshes, in buckets of 512: "
                            "%u %u %u %u %u %u %u %u (last bucket is 3584+); "
                            "%u exceeded the %u capacity",
                            chunkMeshCount, chunkVertexHistogram[0], chunkVertexHistogram[1],
                            chunkVertexHistogram[2], chunkVertexHistogram[3],
                            chunkVertexHistogram[4], chunkVertexHistogram[5],
                            chunkVertexHistogram[6], chunkVertexHistogram[7],
                            chunkOverflowCount, kChunkVertexCapacity);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE dedup ratio %.2fx (%llu indices over %llu vertices); "
                            "a soup reads 1.00, a fully shared surface about 6",
                            totalChunkVertices > 0
                                    ? static_cast<double>(totalChunkIndices) /
                                              static_cast<double>(totalChunkVertices)
                                    : 0.0,
                            static_cast<unsigned long long>(totalChunkIndices),
                            static_cast<unsigned long long>(totalChunkVertices));
    }
}

void OpenChiselWorld::UploadDirtyChunks(uint32_t frameIndex) {
    recon::Reconstruction* reconstruction = recon::reconstruction_holder::Get();
    if (reconstruction == nullptr) {
        return;
    }

    for (auto& [key, entry] : chunkNodes) {
        if (entry.uploadsRemaining == 0 || entry.mesh == nullptr || entry.node == nullptr) {
            continue;
        }

        if (entry.vertexCount == 0 || entry.indexCount == 0) {
            entry.uploadsRemaining = 0;
            continue;
        }

        vertexScratch.resize(kChunkVertexCapacity);
        indexScratch.resize(kChunkIndexCapacity);
        if (!reconstruction->WriteChunk(recon::ChunkKey{key.x, key.y, key.z},
                                        vertexScratch.data(), kChunkVertexCapacity,
                                        indexScratch.data(), kChunkIndexCapacity)) {
            // The chunk went away between Update and here, or it outgrew the
            // capacity. Either way there is nothing to upload and retrying next
            // frame would not help.
            entry.uploadsRemaining = 0;
            continue;
        }

        // The COUNTS, not the scratch buffer's size: WriteChunk fills only what
        // the chunk has, and the rest of the buffer is whatever the last chunk
        // left there.
        //
        // One region per call; see ChunkNode::uploadsRemaining.
        entry.mesh->Update(frameIndex, vertexScratch.data(), entry.vertexCount,
                           indexScratch.data(), entry.indexCount);
        --entry.uploadsRemaining;

        if (!entry.node->renderable.has_value()) {
            entry.node->renderable = Renderable(*entry.mesh);
            entry.node->renderable->material = chunkMaterial;
        }
    }
}

void OpenChiselWorld::Render(const FrameContext& frame) {
    // Chunk geometry first, and it has to be HERE rather than in Update.
    //
    // Update runs before FrameRing::BeginFrame, which is the call that blocks
    // until the slot being reused has retired on the GPU. Writing a mesh region
    // before that returns is a write into memory a draw from two frames ago may
    // still be reading. By the time Render is called the slot is ours, and no
    // pass below has recorded a draw against it yet, which is the only window
    // where both are true.
    //
    // No GpuScope: this is CPU work into mapped memory and records no commands,
    // so a GPU timestamp around it would measure nothing.
    UploadDirtyChunks(frame.frameIndex);

    // The camera image goes up first, and outside any rendering scope, because
    // buffer-to-image copies and image barriers are both illegal inside one.
    // It opens no scope of its own: the final pass is what draws it.
    if (cameraFeed != nullptr) {
        if (const ar::Subsystem* subsystem = ar::subsystem_holder::Get()) {
            // The guard holds the subsystem's lock across the upload. Without it,
            // the activity pausing mid-frame releases the ArImage being copied
            // out of; see AcquireFrame.
            const ar::Subsystem::FrameGuard guard = subsystem->AcquireFrame();
            GpuScope scope(frame, "camera upload");
            cameraFeed->Upload(frame, guard.get());
        }
    }

    // Transparent only when there is actually a feed behind it to reveal.
    const bool hasFeed = cameraFeed != nullptr && cameraFeed->ready();
    {
        GpuScope scope(frame, "mesh");
        meshPass->Render(frame, root(), hasFeed);
    }

    const Image* color = meshPass->colorTarget();
    if (color == nullptr) {
        // The mesh pass gave up on its targets: a window with no area, most
        // likely. Nothing to composite.
        return;
    }
    {
        GpuScope scope(frame, "final");
        finalPass->Render(frame, *color, cameraFeed.get());
    }
}

} // namespace putorana::worlds::openChisel
