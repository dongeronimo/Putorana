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

    /**
     * Ceiling on a voxel's accumulated weight. 0 means unbounded.
     *
     * Unbounded is what upstream does and it is wrong for a scene that changes.
     * A voxel observed three hundred times sits at weight 300, so a fresh
     * observation moves it by 1/301 and the reconstruction can no longer be
     * corrected by new evidence.
     *
     * 200 was chosen against the arithmetic of what one observation is now
     * worth, and that arithmetic is the thing to recheck if this is ever
     * changed. With per-pixel confidence weights one observation contributes at
     * most 1.0, so 200 is roughly seven seconds of continuous full-confidence
     * viewing. A previous attempt at this used 100 while one observation was
     * contributing 8.33 (weight / 2*tau, with tau constant at 0.06), which
     * saturated a voxel in twelve frames and made the surface flicker.
     *
     * This is NOT the noise control. A high ceiling means more averaging and a
     * smoother surface; the ceiling exists so that removal and correction stay
     * possible at all.
     * */
    float maxWeight = 200.0f;

    /**
     * Space carving: clear voxels the sensor has now seen THROUGH.
     *
     * On, and it was off until the mechanism was fixed. Upstream's carve could
     * not remove a surface for two independent reasons, both documented in
     * DistVoxel::Carve. What runs now decays the voxel's accumulated weight and
     * Resets it to unobserved once that falls below the floor, which is the
     * state ChunkManager skips when meshing.
     *
     * carvingDecay is per observation. At 30 fps, 0.85 clears a voxel carried at
     * weight 200 in about a second of looking at where it used to be:
     * 0.85^n * 200 < 0.5 needs n = 37.
     *
     * carvingDistance is the DEAD ZONE above the integration band, not a
     * threshold measured from the surface. Between the end of the band and this
     * much further out, a voxel is neither fused nor eroded. That gap is what
     * stops a voxel on the boundary from being integrated one frame and carved
     * the next as depth noise crosses the line, which showed up on the device as
     * geometry flickering with nothing moving. Larger means slower removal and
     * steadier geometry, and that trade is the whole point of the number.
     * */
    bool carvingEnabled = true;
    float carvingDistance = 0.05f;
    float carvingDecay = 0.85f;
    float carvingMinWeight = 0.5f;

    /**
     * Confidence below which a depth sample is thrown away outright, 0-255.
     *
     * ## This is an outlier filter, NOT the quality control
     *
     * Google's guidance for raw depth is to drop everything below 128, and on
     * this device that was measured rejecting 79% of the samples in a frame.
     * The reconstruction became notably more faithful and completely unusable:
     * a floor came back as a scatter of disconnected islands rather than a
     * surface. That is not a threshold that needs adjusting, it is the wrong
     * instrument. A floor is exactly the low-texture surface the confidence map
     * is most pessimistic about, so a gate high enough to suppress the bad
     * geometry also deletes the good.
     *
     * Quality is handled by confidenceWeightFloor below, which lets poor samples
     * still build a continuous surface while being outvoted where better ones
     * exist. What remains for this number is the job a weight cannot do: keeping
     * the worst readings out of the FRUSTUM. The far plane Chisel integrates
     * against comes from the furthest surviving sample (see Reconstruction.cpp),
     * and the wildest readings in a raw frame -- the 13 m one in a one metre
     * room that put the process past 1.1 GB -- are the near-zero confidence
     * ones. Low enough to keep the floor, high enough to keep the garbage out.
     *
     * 0 disables it and fuses everything, which is what the app did before this
     * existed.
     * */
    uint8_t confidenceThreshold = 32;

    /**
     * The weight the LEAST confident surviving sample gets, relative to 1.0 for
     * a fully confident one.
     *
     * Not zero, and that is the whole design. A sample at confidence 40 is worth
     * far less than one at 240, but it is worth much more than nothing when it
     * is the only observation of that patch of floor, and a TSDF fuses by
     * weighted average, so a small weight is exactly the way to say "use this
     * unless something better turns up". Zero here reduces the mechanism back to
     * the gate that produced the disconnected islands.
     * */
    float confidenceWeightFloor = 0.05f;

    /**
     * Shape of the curve between that floor and 1.0, as an exponent on the
     * normalised confidence. 1.0 is a straight line; higher is convex, pushing
     * weak samples down hard while leaving confident ones near 1.
     *
     * 2.0 rather than 1.0 because the confidence distribution turned out to be
     * BIMODAL once it was measured. Roughly 40% of samples land in the bottom
     * bucket and 34% in the top, with the six buckets between them holding 3% to
     * 6% each. A straight line spends nearly its whole range on that thin middle
     * and separates the two modes by only about 5.5x, so six weak samples
     * outvote one good one. Squaring takes the separation to about 13.5x without
     * moving the confident end:
     *
     *     confidence 60  ->  linear 0.17,  squared 0.065
     *     confidence 240 ->  linear 0.94,  squared 0.88
     *
     * The case this is aimed at is a patch receiving BOTH kinds of sample,
     * mostly weak with the occasional good one. A patch that only ever receives
     * weak samples is still reconstructed from them, which is the intent.
     * */
    float confidenceWeightExponent = 2.0f;

    /**
     * Depth range to trust, in metres.
     *
     * The far plane is the most expensive number in this struct, and not because
     * of quality. Every frame the integrator visits every chunk whose box meets
     * the view frustum and projects all 4096 of its voxels, whether or not any
     * surface is near them -- so the per-frame cost tracks the VOLUME of the
     * frustum, which grows with the cube of this. Dropping 5 m to 3 m leaves
     * (3/5)^3 = 22% of the volume.
     *
     * 3 m is chosen from the scene rather than by tuning: this is a room-scale
     * reconstruction of things within arm's reach to across the room, ARCore's
     * raw depth on interior surfaces is already unreliable well before 5 m, and
     * a wall at 4 m contributes samples too coarse to mesh usefully at 4 cm
     * voxels. Raising it back is a real capability change, not a knob.
     * */
    float nearPlane = 0.3f;
    float farPlane = 3.0f;
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
