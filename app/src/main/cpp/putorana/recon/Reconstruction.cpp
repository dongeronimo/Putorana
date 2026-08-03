#include "Reconstruction.h"

#include <open_chisel/Chisel.h>
#include <open_chisel/ProjectionIntegrator.h>
#include <open_chisel/camera/DepthImage.h>
#include <open_chisel/camera/PinholeCamera.h>

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace putorana::recon {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

chisel::ChunkID ToChunkId(const ChunkKey& key) {
    return {key.x, key.y, key.z};
}

ChunkKey ToChunkKey(const chisel::ChunkID& id) {
    return ChunkKey{id.x(), id.y(), id.z()};
}

/**
 * ARCore's camera-to-world pose, as the extrinsic OpenChisel wants.
 *
 * Two conventions meet here and they disagree on two axes. The intrinsics --
 * and therefore everything ProjectionIntegrator does with them -- are computer
 * vision convention: +X right, +Y DOWN, +Z FORWARD out of the lens. That is why
 * the integrator rejects points with `voxelCenterInCamera.z() < 0` and reads
 * depth off `.z()` directly. ARCore's pose is OpenGL convention: +Y UP, camera
 * looking down its own -Z.
 *
 * So the transform is the pose composed with a flip of Y and Z. This is exactly
 * the same pair of sign flips Unproject.cpp applies per point, written once as a
 * matrix -- and if the two ever disagree, the point cloud and the reconstruction
 * will land in different places, which is the cheapest possible way to notice.
 * */
chisel::Transform ToExtrinsic(const ar::CameraPose& pose) {
    // glm's quat constructor takes w FIRST; ARCore's raw array has it LAST.
    const Eigen::Quaternionf rotation(pose.rotation[3], pose.rotation[0], pose.rotation[1],
                                      pose.rotation[2]);

    chisel::Transform transform = chisel::Transform::Identity();
    transform.linear() = rotation.toRotationMatrix() *
                         Eigen::DiagonalMatrix<float, 3>(1.0f, -1.0f, -1.0f);
    transform.translation() =
            Eigen::Vector3f(pose.translation[0], pose.translation[1], pose.translation[2]);
    return transform;
}

} // namespace

struct Reconstruction::Impl {
    Config config;

    std::unique_ptr<chisel::Chisel> chisel;
    chisel::ProjectionIntegrator integrator;
    chisel::PinholeCamera camera;

    /**
     * Reused across frames. chisel::DepthImage OWNS its buffer -- its destructor
     * does `delete[] data` -- so pointing it at ARCore's memory with SetData
     * would be a double free the moment the frame is released. Allocated once at
     * the depth map's size and written into instead.
     * */
    std::shared_ptr<chisel::DepthImage<float>> depth;

    std::mutex meshMutex;

    std::vector<ChunkUpdate> changed;
    std::vector<ChunkKey> removed;

    /** Chunk IDs whose mesh was non-empty last time we looked, so removals can
     *  be reported even though OpenChisel garbage-collects silently. */
    std::unordered_map<chisel::ChunkID, bool, chisel::ChunkHasher> live;

    bool loggedFirstIntegration = false;

    /** Chunk origin from its key alone: numVoxels * ID * resolution, which is
     *  what Chunk's constructor computes. Deterministic, so it needs no lookup
     *  and works for a chunk that has already been collected. */
    glm::vec3 OriginOf(const chisel::ChunkID& id) const {
        const float edge = static_cast<float>(config.chunkVoxels) * config.voxelMetres;
        return glm::vec3(static_cast<float>(id.x()) * edge, static_cast<float>(id.y()) * edge,
                         static_cast<float>(id.z()) * edge);
    }
};

Reconstruction::Reconstruction() : impl_(std::make_unique<Impl>()) {}
Reconstruction::~Reconstruction() = default;

std::unique_ptr<Reconstruction> Reconstruction::Create(const Config& config, std::string& error) {
    if (config.chunkVoxels <= 0 || config.voxelMetres <= 0.0f) {
        error = "Reconstruction: chunkVoxels and voxelMetres must both be positive";
        return nullptr;
    }

    auto reconstruction = std::unique_ptr<Reconstruction>(new Reconstruction());
    Impl& impl = *reconstruction->impl_;
    impl.config = config;

    const Eigen::Vector3i chunkSize(config.chunkVoxels, config.chunkVoxels, config.chunkVoxels);
    // useColor = false. Colour would double the per-voxel cost -- a ColorVoxel is
    // as wide as a DistVoxel even after the fork trimmed both -- and nothing
    // draws the reconstruction in colour. See third_party/open_chisel/README.md.
    impl.chisel = std::make_unique<chisel::Chisel>(chunkSize, config.voxelMetres, false);

    // Centroids are the per-voxel offsets within a chunk, precomputed once by
    // ChunkManager. The integrator walks them for every chunk in the frustum, so
    // it needs its own copy rather than reaching for the manager per frame.
    impl.integrator.SetCentroids(impl.chisel->GetChunkManager().GetCentroids());
    impl.integrator.SetTruncator(chisel::Truncator(config.truncationQuadratic,
                                                   config.truncationLinear,
                                                   config.truncationConstant,
                                                   config.truncationScale));
    impl.integrator.SetWeighter(chisel::Weighter(config.weight));
    impl.integrator.SetCarvingDist(config.carvingDistance);
    impl.integrator.SetCarvingEnabled(config.carvingEnabled);

    impl.camera.SetNearPlane(config.nearPlane);
    impl.camera.SetFarPlane(config.farPlane);

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "RECONPROBE reconstruction ready: %d^3 voxels per chunk at %.3f m "
                        "(%.2f m chunks), truncation |%.3g d^2 + %.3g d + %.3g| * %.3g, "
                        "carving %s",
                        config.chunkVoxels, config.voxelMetres,
                        config.chunkVoxels * config.voxelMetres, config.truncationQuadratic,
                        config.truncationLinear, config.truncationConstant, config.truncationScale,
                        config.carvingEnabled ? "on" : "off");
    return reconstruction;
}

void Reconstruction::Integrate(const ar::DepthImage& depth, const ar::CameraPose& pose) {
    Impl& impl = *impl_;
    if (depth.millimetres == nullptr || depth.width <= 0 || depth.height <= 0) {
        return;
    }
    const ar::Intrinsics& k = depth.intrinsics;
    if (k.fx <= 0.0f || k.fy <= 0.0f) {
        return;
    }

    if (impl.depth == nullptr || impl.depth->GetWidth() != depth.width ||
        impl.depth->GetHeight() != depth.height) {
        impl.depth = std::make_shared<chisel::DepthImage<float>>(depth.width, depth.height);
    }

    // --- millimetres to metres, and zero to NaN ---
    //
    // The second half is not cosmetic. ARCore writes 0 where it has no estimate,
    // and ProjectionIntegrator's only guard is `if (std::isnan(depth)) continue`.
    // Passing 0.0f straight through would tell it there is a surface at zero
    // distance for every pixel it could not measure -- which on a typical indoor
    // frame is a real fraction of the image, and would carve a hole through the
    // reconstruction wherever the sensor was uncertain.
    float* out = impl.depth->GetMutableData();
    const auto* base = reinterpret_cast<const uint8_t*>(depth.millimetres);
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    for (int32_t row = 0; row < depth.height; ++row) {
        // Through the byte stride, not the width: they are equal on the device
        // this was written against and nothing promises they stay that way.
        const auto* source = reinterpret_cast<const uint16_t*>(base + row * depth.rowStrideBytes);
        float* destination = out + static_cast<size_t>(row) * depth.width;
        for (int32_t column = 0; column < depth.width; ++column) {
            const uint16_t millimetres = source[column];
            destination[column] = millimetres == 0 ? kNaN
                                                   : static_cast<float>(millimetres) * 0.001f;
        }
    }

    // Re-read every frame: the header says intrinsics may change per frame, and
    // autofocus moves the focal length.
    chisel::Intrinsics intrinsics;
    intrinsics.SetFx(k.fx);
    intrinsics.SetFy(k.fy);
    intrinsics.SetCx(k.cx);
    intrinsics.SetCy(k.cy);
    impl.camera.SetIntrinsics(intrinsics);
    impl.camera.SetWidth(depth.width);
    impl.camera.SetHeight(depth.height);

    impl.chisel->IntegrateDepthScan<float>(impl.integrator, impl.depth, ToExtrinsic(pose),
                                           impl.camera);

    if (!impl.loggedFirstIntegration) {
        impl.loggedFirstIntegration = true;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "RECONPROBE first integration done: %zu chunks, %zu awaiting remesh",
                            impl.chisel->GetChunkManager().GetChunks().size(),
                            impl.chisel->GetMeshesToUpdate().size());
    }
}

uint32_t Reconstruction::Remesh(uint32_t maxChunks) {
    Impl& impl = *impl_;
    if (maxChunks == 0) {
        return 0;
    }

    chisel::ChunkSet& pending = impl.chisel->GetMutableMeshesToUpdate();
    chisel::ChunkManager& chunks = impl.chisel->GetMutableChunkManager();

    uint32_t done = 0;
    // Taken from the front and erased as we go, so what is left stays marked for
    // the next frame. A chunk that no longer exists is dropped rather than
    // meshed: integration marks a 3x3x3 neighbourhood, which reaches chunks that
    // were never allocated.
    for (auto it = pending.begin(); it != pending.end() && done < maxChunks;) {
        const chisel::ChunkID id = it->first;
        it = pending.erase(it);

        if (!chunks.HasChunk(id)) {
            continue;
        }
        chunks.RecomputeMesh(id, impl.meshMutex);
        ++done;

        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        const chisel::MeshMap& meshes = chunks.GetAllMeshes();
        const auto found = meshes.find(id);
        if (found != meshes.end() && found->second != nullptr) {
            vertexCount = static_cast<uint32_t>(found->second->vertices.size());
            indexCount = static_cast<uint32_t>(found->second->indices.size());
        }

        if (vertexCount == 0) {
            // Nothing to draw here. Reported as a removal only if it USED to
            // have something, so the renderer is not told to delete a node that
            // never existed.
            if (impl.live.erase(id) > 0) {
                impl.removed.push_back(ToChunkKey(id));
            }
            continue;
        }

        impl.live[id] = true;
        impl.changed.push_back(ChunkUpdate{ToChunkKey(id), impl.OriginOf(id), vertexCount,
                                           indexCount});
    }
    return done;
}

void Reconstruction::CollectUpdates(std::vector<ChunkUpdate>& changed,
                                    std::vector<ChunkKey>& removed) {
    changed.clear();
    removed.clear();
    changed.swap(impl_->changed);
    removed.swap(impl_->removed);
}

bool Reconstruction::WriteChunk(const ChunkKey& key, Vertex* vertices, uint32_t vertexCapacity,
                                uint32_t* indices, uint32_t indexCapacity) const {
    const Impl& impl = *impl_;
    if (vertices == nullptr || indices == nullptr) {
        return false;
    }

    const chisel::MeshMap& meshes = impl.chisel->GetChunkManager().GetAllMeshes();
    const auto found = meshes.find(ToChunkId(key));
    if (found == meshes.end() || found->second == nullptr) {
        return false;
    }
    const chisel::Mesh& mesh = *found->second;

    const auto vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    const auto indexCount = static_cast<uint32_t>(mesh.indices.size());
    if (vertexCount > vertexCapacity || indexCount > indexCapacity) {
        return false;
    }
    // Upstream asserts this rather than checking it, and our build defines
    // NDEBUG, so the assert is gone. Reading normals past their end would be a
    // silent out-of-bounds read into whatever follows the vector.
    if (mesh.normals.size() != mesh.vertices.size()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "RECONPROBE chunk %d,%d,%d has %zu vertices but %zu normals",
                            key.x, key.y, key.z, mesh.vertices.size(), mesh.normals.size());
        return false;
    }

    // The one place SoA becomes AoS, world becomes chunk-local, and size_t
    // becomes uint32. Writing straight into the caller's memory -- which may be
    // a mapped VkBuffer -- is why this takes a destination instead of returning
    // a span: it makes this the only pass over the data.
    const glm::vec3 origin = impl.OriginOf(ToChunkId(key));
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const chisel::Vec3& position = mesh.vertices[i];
        const chisel::Vec3& normal = mesh.normals[i];
        vertices[i].position =
                glm::vec3(position.x(), position.y(), position.z()) - origin;
        vertices[i].normal = glm::vec3(normal.x(), normal.y(), normal.z());
    }
    for (uint32_t i = 0; i < indexCount; ++i) {
        indices[i] = static_cast<uint32_t>(mesh.indices[i]);
    }
    return true;
}

void Reconstruction::MarkAllDirty() {
    Impl& impl = *impl_;
    impl.changed.clear();
    impl.removed.clear();
    impl.live.clear();

    for (const auto& entry : impl.chisel->GetChunkManager().GetAllMeshes()) {
        if (entry.second == nullptr || entry.second->vertices.empty()) {
            continue;
        }
        impl.live[entry.first] = true;
        impl.changed.push_back(ChunkUpdate{
                ToChunkKey(entry.first), impl.OriginOf(entry.first),
                static_cast<uint32_t>(entry.second->vertices.size()),
                static_cast<uint32_t>(entry.second->indices.size())});
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "RECONPROBE re-reporting %zu chunk meshes after a device rebuild",
                        impl.changed.size());
}

uint32_t Reconstruction::chunkCount() const {
    return static_cast<uint32_t>(impl_->chisel->GetChunkManager().GetChunks().size());
}

uint32_t Reconstruction::pendingRemeshCount() const {
    return static_cast<uint32_t>(impl_->chisel->GetMeshesToUpdate().size());
}

const Config& Reconstruction::config() const {
    return impl_->config;
}

namespace reconstruction_holder {

namespace {
std::unique_ptr<Reconstruction> g_reconstruction;
} // namespace

Reconstruction* Get() {
    return g_reconstruction.get();
}

Reconstruction* Create(const Config& config, std::string& error) {
    if (g_reconstruction == nullptr) {
        g_reconstruction = Reconstruction::Create(config, error);
    }
    return g_reconstruction.get();
}

void Destroy() {
    g_reconstruction.reset();
}

} // namespace reconstruction_holder

} // namespace putorana::recon
