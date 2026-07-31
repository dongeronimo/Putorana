#ifndef PUTORANA_GRAPHICS_GLTFLOADER_H
#define PUTORANA_GRAPHICS_GLTFLOADER_H

#include "Mesh.h"
#include "Node.h"

#include <memory>
#include <string>
#include <vector>

namespace putorana::graphics {

class World;

/**
 * What came out of a file. Two kinds of thing, with two kinds of ownership,
 * which is the loader's whole shape in one struct.
 * */
struct GltfLoadResult {
    /**
     * The file's top-level nodes, DETACHED. The caller decides where they go:
     * straight under the world's ROOT, or kept aside as a prefab template to be
     * cloned. Returning them parented would take that choice away.
     * */
    std::vector<std::unique_ptr<Node>> roots;

    /**
     * Every node created, in pre-order (parents before children). Non-owning —
     * the tree above owns them. This is how a caller finds "Wall00" in the file
     * to turn it into a prefab.
     * */
    std::vector<Node*> nodes;

    /**
     * The unique meshes, already uploaded. Non-owning: they were handed to the
     * World on the way through, because a mesh is a shared asset and the world
     * is what outlives any particular node that draws it.
     * */
    std::vector<Mesh*> meshes;
};

/**
 * Reads a .glb out of the APK's assets and turns its scene into our node graph.
 *
 * ## What maps to what
 *
 *  - an assimp node becomes a Node, with its transform decomposed back into
 *    position/rotation/scale;
 *  - a mesh on that node becomes a Renderable on it. A node carrying SEVERAL
 *    meshes — glTF calls them primitives, and they exist because each one can
 *    have its own material — becomes the node plus one child per extra, named
 *    "<node>_prim1" and so on, because a Renderable is one per Node;
 *  - a mesh referenced by several nodes is uploaded ONCE and pointed at by
 *    several Renderables.
 *
 * ## What this pass deliberately does not read
 *
 *  - MATERIALS. Renderables come back with a null material.
 *  - SKINS. A mesh with bones is loaded as static geometry in its bind pose and
 *    says so in the log. Its bone indices address a joint array that only the
 *    skin defines, so extracting the weights before there is a Skin would mean
 *    inventing that ordering twice.
 *  - animations, which need a clip type that does not exist and are the other
 *    half of skinning.
 *  - cameras and lights. The file has them and assimp exposes them, but the
 *    renderer this is ported from builds those in code rather than reading them,
 *    and assimp's field-of-view conversion is not something to trust unverified.
 *
 * `assetPath` is relative to the assets root, e.g. "unitary_cube.glb". Only .glb
 * — the binary, self-contained form — is supported, because the file is parsed
 * from a memory buffer and a plain .gltf would send the parser looking for
 * sibling files that do not exist inside an APK.
 * */
bool LoadGltf(World& world, const std::string& assetPath, GltfLoadResult& result,
              std::string& error);

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_GLTFLOADER_H
