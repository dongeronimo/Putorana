#ifndef PUTORANA_GRAPHICS_SPACECHUNK_H
#define PUTORANA_GRAPHICS_SPACECHUNK_H

#include <cstddef>
#include <cstdint>

namespace putorana::graphics {

/**
 * Present on a node means the node IS a chunk of reconstructed space, and these
 * are its coordinates on the reconstruction's chunk grid.
 *
 * ## Why the scene graph carries this at all
 *
 * The reconstruction is a sparse spatial structure and so is a scene graph, and
 * the two want the same things from it: find what is near here, cull what is not
 * in view, update one piece without touching the rest. Rather than keep a second
 * structure alongside the node tree, the node tree IS the structure — one node
 * per chunk, positioned at the chunk's origin, holding that chunk's mesh.
 *
 * That buys frustum culling for the reconstruction for free, since the pass
 * already culls nodes. At 4cm voxels a room is a few hundred chunks and only a
 * fraction are in view at once, which is the difference between a comfortable
 * draw call count and an uncomfortable one.
 *
 * ## What it is NOT
 *
 * It is not the reconstruction's own map. putorana::recon keeps a hash of chunks
 * holding voxels, and that one cannot move here: meshing a chunk reads its
 * neighbours' voxels to close the seams between them, so the TSDF's hash is part
 * of the algorithm rather than storage the renderer could own.
 *
 * More importantly, nodes are DERIVED and rebuildable. A World belongs to a
 * Device, and a Device dies every time the app is backgrounded — so every node
 * here evaporates while the reconstruction survives at process scope. Anything
 * that only exists on a node is lost on the first press of the Home button. The
 * coordinates below are enough to ask the reconstruction for the geometry again,
 * and that is the whole reason they live on the node rather than in a map that
 * would have to be rebuilt in step with it.
 *
 * ## Plain ints, deliberately
 *
 * Not recon::ChunkKey. The core scene graph type has no reason to depend on the
 * reconstruction, and the conversion is three integers at the seam.
 * */
struct SpaceChunk {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    friend bool operator==(const SpaceChunk& a, const SpaceChunk& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend bool operator!=(const SpaceChunk& a, const SpaceChunk& b) { return !(a == b); }
};

/**
 * Teschner's spatial hash — three large primes, one XOR — which is the same
 * function OpenChisel's ChunkHasher uses.
 *
 * Matching it is not required for correctness, since the two maps are separate,
 * but it is worth doing: the function is chosen so that chunks adjacent in space
 * do not collide, which is exactly the access pattern both sides have. A generic
 * hash of three ints has no such property and would cluster.
 * */
struct SpaceChunkHash {
    static constexpr size_t p1 = 73856093;
    static constexpr size_t p2 = 19349663;
    static constexpr size_t p3 = 83492791;

    size_t operator()(const SpaceChunk& chunk) const {
        return (static_cast<size_t>(chunk.x) * p1) ^ (static_cast<size_t>(chunk.y) * p2) ^
               (static_cast<size_t>(chunk.z) * p3);
    }
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_SPACECHUNK_H
