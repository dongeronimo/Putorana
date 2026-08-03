#ifndef PUTORANA_RECON_RECONSTRUCTION_H
#define PUTORANA_RECON_RECONSTRUCTION_H

#include "ar/Subsystem.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * The reconstruction: depth frames in, chunk meshes out.
 *
 * ## Nothing here mentions Eigen, and that is load bearing
 *
 * OpenChisel is entirely Eigen-typed and lives behind this header, reached only
 * through the pimpl below. The rule is not stylistic: this library is compiled
 * with NDEBUG and -O3 so Eigen is not catastrophically slow in a Debug build,
 * and Eigen turns NDEBUG into EIGEN_NO_DEBUG, which changes the body of its
 * inline template code. An Eigen type crossing this header would let a caller
 * compiled without those flags emit a second, different definition of the same
 * weak symbol -- an ODR violation whose symptom is that you sometimes get the
 * checked version and sometimes the fast one, with no pattern.
 *
 * See recon/README.md, "The boundary, and why it is also a build setting".
 * */
namespace putorana::recon {

/** A chunk's coordinates on the reconstruction grid. Eigen::Vector3i inside. */
struct ChunkKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    friend bool operator==(const ChunkKey& a, const ChunkKey& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

/**
 * 24 bytes, laid out to match graphics::PositionNormalVertex exactly so the
 * interleave below writes straight into a mapped vertex buffer.
 *
 * Declared here rather than reusing the graphics type because graphics/Mesh.h
 * includes volk.h, and this namespace inherits putorana::ar's rule that it may
 * not. The two are kept honest by a static_assert at the seam.
 * */
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
};
static_assert(sizeof(Vertex) == 24, "recon::Vertex must stay tightly packed at 24 bytes");

/**
 * What changed about one chunk, without the geometry.
 *
 * Cheap on purpose: the caller reads this, sizes and maps a buffer, and then
 * asks WriteChunk to interleave directly into it. Handing out the geometry here
 * instead would mean an extra full copy of every remeshed chunk, every frame.
 * */
struct ChunkUpdate {
    ChunkKey key;

    /**
     * Where this chunk's node goes. Vertices are CHUNK LOCAL -- this origin is
     * already subtracted from them -- so the node's transform is what puts the
     * geometry back in the world.
     *
     * That is not only tidiness. A room is tens of metres across, and float32
     * near 30.0 has about 2 micrometres of precision left after the exponent
     * takes its share; near zero it has far more. Chunk-local vertices keep the
     * sub-millimetre detail marching cubes worked to produce.
     * */
    glm::vec3 origin;

    /** Zero means the chunk now has no surface in it; drop its node. */
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
};

/** Everything that has to be decided before the first frame. */
struct Config {
    /** Voxels per chunk edge. Sets draw-call count and remesh granularity. */
    int32_t chunkVoxels = 16;

    /** Metres per voxel. With chunkVoxels above, 16 * 0.04 = a 64cm chunk. */
    float voxelMetres = 0.04f;

    /**
     * Truncation: tau(d) = |a*d^2 + b*d + c| * s, evaluated per voxel against
     * the depth reading d. The quadratic term is what makes distant samples
     * carve a wider, softer band -- see README.md, "Where the distance-dependence
     * actually lives". Constant-only (a = b = 0) is the degenerate case.
     * */
    float truncationQuadratic = 0.0f;
    float truncationLinear = 0.0f;
    float truncationConstant = 0.06f;
    float truncationScale = 1.0f;

    /** Numerator of w = weight / (2 * tau). */
    float weight = 1.0f;

    /** Space carving: clear voxels seen THROUGH. Costs time, removes ghosts. */
    bool carvingEnabled = false;
    float carvingDistance = 0.05f;

    /** Depth range to trust, in metres. ARCore's useful band is roughly this. */
    float nearPlane = 0.3f;
    float farPlane = 5.0f;
};

class Reconstruction {
public:
    static std::unique_ptr<Reconstruction> Create(const Config& config, std::string& error);
    ~Reconstruction();

    Reconstruction(const Reconstruction&) = delete;
    Reconstruction& operator=(const Reconstruction&) = delete;

    /**
     * Fuse one depth frame. `pose` must be the SENSOR pose, for the same reason
     * Unproject requires it: it is the one whose axes agree with the unrotated
     * intrinsics the depth map carries. Does nothing if the frame has no depth.
     *
     * Cheaper than Remesh, and it does not have to run at the same rate --
     * depth is highly redundant frame to frame, so integrating at 5-10 Hz costs
     * almost nothing in quality and is the first lever to pull if this ever
     * shows up in a profile.
     * */
    void Integrate(const ar::DepthImage& depth, const ar::CameraPose& pose);

    /**
     * Re-run marching cubes on at most `maxChunks` dirty chunks. Returns how
     * many it did.
     *
     * Budgeted deliberately. Integration marks the whole 3x3x3 neighbourhood of
     * every chunk it touches -- meshing reads neighbours to close the seams
     * between chunks -- so sweeping a room dirties hundreds at once, and doing
     * all of them in one frame is a dropped frame. What is left stays marked.
     * */
    uint32_t Remesh(uint32_t maxChunks);

    /**
     * Hand over what changed since the last call, and clear it.
     *
     * `removed` are chunks that no longer exist: garbage collected because they
     * turned out to hold no surface. Their nodes should be destroyed.
     * */
    void CollectUpdates(std::vector<ChunkUpdate>& changed, std::vector<ChunkKey>& removed);

    /**
     * Interleave one chunk's geometry into caller-provided memory, which may be
     * a mapped VkBuffer.
     *
     * The counts must be at least those the matching ChunkUpdate reported.
     * Returns false if the chunk is gone or the capacities are too small, having
     * written nothing.
     *
     * Caller-provided rather than a span handed out, so the interleave from
     * OpenChisel's separate vertex and normal arrays lands in GPU-visible memory
     * in one pass instead of being copied again afterwards.
     * */
    bool WriteChunk(const ChunkKey& key, Vertex* vertices, uint32_t vertexCapacity,
                    uint32_t* indices, uint32_t indexCapacity) const;

    /**
     * Re-report every chunk that has geometry, as if it had just changed.
     *
     * For after a Device rebuild. A World belongs to a Device and a Device dies
     * every time the app is backgrounded, so every node holding a chunk mesh is
     * destroyed while this object -- at process scope -- keeps the reconstruction.
     * This is how the renderer asks for all of it back.
     * */
    void MarkAllDirty();

    /** Chunks currently holding voxels, and chunks still waiting to be remeshed. */
    uint32_t chunkCount() const;
    uint32_t pendingRemeshCount() const;

    const Config& config() const;

private:
    Reconstruction();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * The one reconstruction, at PROCESS scope.
 *
 * Not owned by Device, and this is the same rule ar::Subsystem follows. A Device
 * is destroyed every time the app is backgrounded; a reconstruction that went
 * with it would throw away the mapped world on every press of the Home button,
 * which is the one thing in this app that must survive.
 * */
namespace reconstruction_holder {

Reconstruction* Get();
Reconstruction* Create(const Config& config, std::string& error);
void Destroy();

} // namespace reconstruction_holder

} // namespace putorana::recon

#endif // PUTORANA_RECON_RECONSTRUCTION_H
