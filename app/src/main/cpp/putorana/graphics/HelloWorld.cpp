#include "HelloWorld.h"

#include "ChunkMaterial.h"
#include "Device.h"
#include "FrameRing.h"
#include "Swapchain.h"


#include <algorithm>

#include <android/log.h>

#include <utility>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";
constexpr const char* kChunkMaterialName = "ReconChunk";

/**
 * Chunks remeshed per frame. Integration marks the whole 3x3x3 neighbourhood of
 * every chunk it touches, so a sweep of a room dirties hundreds at once and
 * marching cubes on all of them inside one frame is a dropped frame. What is not
 * done stays marked.
 * */
constexpr uint32_t kRemeshBudgetPerFrame = 8;

/**
 * Vertices a chunk mesh is allocated for. A GUESS, and the histogram below is
 * what replaces it with a measurement.
 *
 * It matters because MeshStorage::Mutable fixes capacity at creation: too small
 * and dense chunks are dropped, too large and most of the biggest allocation in
 * the app is empty. A 16^3 chunk can in theory emit tens of thousands of
 * vertices; a chunk holding one wall emits a few hundred.
 * */
constexpr uint32_t kChunkVertexCapacity = 4096;
constexpr uint32_t kChunkIndexCapacity = kChunkVertexCapacity * 3;

} // namespace

bool HelloWorld::CreateRenderPasses(const Swapchain& swapchain, std::string& error) {
    // The mesh pass draws into its OWN target, in the swapchain's format so the
    // composite is a straight copy. The day tone mapping matters this becomes a
    // float format and only the final pass has to know.
    meshPass_ = MeshPass::Create(device(), swapchain.format(), error);
    if (meshPass_ == nullptr) {
        return false;
    }

    // Not a pass — it draws nothing. It only owns the camera's two textures, for
    // the final pass to composite. Its failure is not the world's: no background
    // still leaves the reconstruction drawn on the clear colour, which is worth
    // having. So the error is logged and swallowed here rather than returned.
    std::string cameraError;
    cameraFeed_ = CameraFeed::Create(device(), cameraError);
    if (cameraFeed_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "no camera background: %s",
                            cameraError.c_str());
    }

    finalPass_ = FinalPass::Create(device(), swapchain.format(), error);
    return finalPass_ != nullptr;
}

bool HelloWorld::CreateWorld(std::string& error) {
    // No cube any more.
    //
    // It was scaffolding and it did its job twice: first as the thing that
    // proved geometry was arriving with the right winding, normals and depth
    // order, then as the marker that proved the unprojection maths — the frozen
    // marker staying nailed to a wall while the phone moved is what ruled out
    // both axis flips, the quaternion order and the choice of pose. The
    // reconstruction now tests all of that continuously and far harder, so a
    // spinning cube in the middle of it is just something in the way.

    // The camera is the only node this world builds. Everything else in the
    // scene arrives from the reconstruction, one node per chunk.
    Node* eye = root().AddChild(Node::Create("camera"));
    cameraNode_ = eye;

    // Which KIND of camera is the one decision this world makes about AR. With a
    // session, ARCore owns both the projection and the pose, and the node's
    // transform becomes an output rather than something set here.
    if (ar::Subsystem* subsystem = ar::subsystem_holder::Get()) {
        // The reconstruction, and the material its chunks are drawn with.
        //
        // reconstruction_holder, not a member: this world dies with the Device
        // every time the app is backgrounded, and the reconstruction must not.
        // Create is a no-op when one already exists, which is exactly what
        // coming back from background looks like.
        auto chunkMaterial = ChunkMaterial::Create(
                device(), glm::vec4(0.45f, 0.72f, 0.95f, 1.0f), error);
        if (chunkMaterial == nullptr) {
            return false;
        }
        chunkMaterial_ = AddMaterial(kChunkMaterialName, std::move(chunkMaterial));

        recon::Config reconConfig;
        if (recon::reconstruction_holder::Create(reconConfig, error) == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "reconstruction: %s", error.c_str());
        } else if (recon::Reconstruction* reconstruction =
                           recon::reconstruction_holder::Get()) {
            // Everything it already has, as if it had just changed. On a first
            // run this is empty; after a trip to background it is the whole map,
            // because every node holding it was destroyed with the Device.
            reconstruction->MarkAllDirty();
        }

        auto camera = ArCamera::Create();
        // ARCore is the one that builds the projection, so it is the one that
        // needs the clip planes. Setting them on the camera alone would change
        // nothing at all.
        subsystem->SetClipPlanes(camera->nearPlane, camera->farPlane);
        arCamera_ = camera.get();
        eye->camera = std::move(camera);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "HelloWorld ready: AR camera, reconstruction on");
    } else {
        // No ARCore means no depth, so nothing will ever be reconstructed and
        // this scene stays empty — the camera feed is gone too, so the screen is
        // the clear colour and nothing else. That is now the honest picture of
        // what this app can do on such a device, where before it was a cube.
        eye->camera = PerspectiveCamera::Create();
        eye->position = glm::vec3(2.0f, 3.0f, 5.0f);
        // LookAt reads the node's world matrix, and it is fresh here because
        // ComputeWorldMatrix walks the parent chain rather than trusting the
        // cache — which has not been filled yet, since no frame has run.
        eye->LookAt(glm::vec3(0.0f, 0.0f, 0.0f));
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "HelloWorld ready: no ARCore, so nothing to reconstruct and nothing "
                            "to draw");
    }
    return true;
}

void HelloWorld::Update(float deltaSeconds) {
    // The AR camera first, because everything else this frame is measured
    // against where the device is. Frame.cpp already ticked the session before
    // calling this, so the frame here is the current one.
    //
    // Under the lock, for the same reason the upload is: Pause arrives on the UI
    // thread and can invalidate the frame mid-read.
    if (arCamera_ != nullptr && cameraNode_ != nullptr) {
        if (const ar::Subsystem* subsystem = ar::subsystem_holder::Get()) {
            const ar::Subsystem::FrameGuard guard = subsystem->AcquireFrame();
            if (const ar::CameraFrame* arFrame = guard.get()) {
                arCamera_->SetFrame(*arFrame, *cameraNode_);

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
    // reconstruction runs above rather than in Render — it creates nodes.
    World::Update(deltaSeconds);
}

void HelloWorld::UpdateReconstruction(const ar::CameraFrame& frame) {
    recon::Reconstruction* reconstruction = recon::reconstruction_holder::Get();
    if (reconstruction == nullptr || chunkMaterial_ == nullptr || !frame.tracking) {
        return;
    }

    reconstruction->Integrate(frame.depth, frame.sensorPose);
    reconstruction->Remesh(kRemeshBudgetPerFrame);
    reconstruction->CollectUpdates(chunkChanged_, chunkRemoved_);

    for (const recon::ChunkKey& key : chunkRemoved_) {
        const auto found = chunkNodes_.find(SpaceChunk{key.x, key.y, key.z});
        if (found == chunkNodes_.end()) {
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

    for (const recon::ChunkUpdate& update : chunkChanged_) {
        const SpaceChunk key{update.key.x, update.key.y, update.key.z};
        ChunkNode& entry = chunkNodes_[key];

        if (entry.node == nullptr) {
            Node* node = root().AddChild(Node::Create("chunk"));
            node->position = update.origin;
            node->spaceChunk = key;
            entry.node = node;
        }

        if (entry.mesh == nullptr) {
            MeshDesc desc;
            desc.name = "chunk";
            desc.format = VertexFormat::PositionNormal;
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
        const size_t bucket = std::min<size_t>(chunkVertexHistogram_.size() - 1,
                                               update.vertexCount / 512);
        ++chunkVertexHistogram_[bucket];
        ++chunkMeshCount_;
        if (update.vertexCount > kChunkVertexCapacity ||
            update.indexCount > kChunkIndexCapacity) {
            ++chunkOverflowCount_;
            // Skipped rather than truncated: half a chunk of geometry is a hole
            // with a torn edge, which reads as a reconstruction bug rather than
            // as the capacity problem it is.
            continue;
        }

        entry.vertexCount = update.vertexCount;
        entry.indexCount = update.indexCount;
        entry.uploadsRemaining = FrameRing::kFramesInFlight;
    }

    if (!loggedChunkHistogram_ && chunkMeshCount_ >= 200) {
        loggedChunkHistogram_ = true;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE chunk vertex counts over %u remeshes, in buckets of 512: "
                            "%u %u %u %u %u %u %u %u (last bucket is 3584+); "
                            "%u exceeded the %u capacity",
                            chunkMeshCount_, chunkVertexHistogram_[0], chunkVertexHistogram_[1],
                            chunkVertexHistogram_[2], chunkVertexHistogram_[3],
                            chunkVertexHistogram_[4], chunkVertexHistogram_[5],
                            chunkVertexHistogram_[6], chunkVertexHistogram_[7],
                            chunkOverflowCount_, kChunkVertexCapacity);
    }
}

void HelloWorld::UploadDirtyChunks(uint32_t frameIndex) {
    recon::Reconstruction* reconstruction = recon::reconstruction_holder::Get();
    if (reconstruction == nullptr) {
        return;
    }

    for (auto& [key, entry] : chunkNodes_) {
        if (entry.uploadsRemaining == 0 || entry.mesh == nullptr || entry.node == nullptr) {
            continue;
        }

        if (entry.vertexCount == 0 || entry.indexCount == 0) {
            entry.uploadsRemaining = 0;
            continue;
        }

        vertexScratch_.resize(kChunkVertexCapacity);
        indexScratch_.resize(kChunkIndexCapacity);
        if (!reconstruction->WriteChunk(recon::ChunkKey{key.x, key.y, key.z},
                                        vertexScratch_.data(), kChunkVertexCapacity,
                                        indexScratch_.data(), kChunkIndexCapacity)) {
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
        // One region per call — see ChunkNode::uploadsRemaining.
        entry.mesh->Update(frameIndex, vertexScratch_.data(), entry.vertexCount,
                           indexScratch_.data(), entry.indexCount);
        --entry.uploadsRemaining;

        if (!entry.node->renderable.has_value()) {
            entry.node->renderable = Renderable(*entry.mesh);
            entry.node->renderable->material = chunkMaterial_;
        }
    }
}

void HelloWorld::Render(const FrameContext& frame) {
    // Chunk geometry first, and it has to be HERE rather than in Update.
    //
    // Update runs before FrameRing::BeginFrame, which is the call that blocks
    // until the slot being reused has retired on the GPU. Writing a mesh region
    // before that returns is a write into memory a draw from two frames ago may
    // still be reading. By the time Render is called the slot is ours, and no
    // pass below has recorded a draw against it yet — which is the only window
    // where both are true.
    //
    // No GpuScope: this is CPU work into mapped memory and records no commands,
    // so a GPU timestamp around it would measure nothing.
    UploadDirtyChunks(frame.frameIndex);

    // The camera image goes up first, and outside any rendering scope, because
    // buffer-to-image copies and image barriers are both illegal inside one.
    // It opens no scope of its own — the final pass is what draws it.
    if (cameraFeed_ != nullptr) {
        if (const ar::Subsystem* subsystem = ar::subsystem_holder::Get()) {
            // The guard holds the subsystem's lock across the upload. Without it,
            // the activity pausing mid-frame releases the ArImage being copied
            // out of — see AcquireFrame.
            const ar::Subsystem::FrameGuard guard = subsystem->AcquireFrame();
            GpuScope scope(frame, "camera upload");
            cameraFeed_->Upload(frame, guard.get());
        }
    }

    // Transparent only when there is actually a feed behind it to reveal.
    const bool hasFeed = cameraFeed_ != nullptr && cameraFeed_->ready();
    {
        GpuScope scope(frame, "mesh");
        meshPass_->Render(frame, root(), hasFeed);
    }

    const Image* color = meshPass_->colorTarget();
    if (color == nullptr) {
        // The mesh pass gave up on its targets — a window with no area, most
        // likely. Nothing to composite.
        return;
    }
    {
        GpuScope scope(frame, "final");
        finalPass_->Render(frame, *color, cameraFeed_.get());
    }
}

} // namespace putorana::graphics
