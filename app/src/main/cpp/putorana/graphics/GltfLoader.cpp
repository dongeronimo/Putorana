#include "GltfLoader.h"

#include "Device.h"
#include "World.h"
#include "putorana/Assets.h"

#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Importer.hpp>

#include <android/log.h>

#include <unordered_map>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * Below this angle between two faces, a generated normal is shared; above it,
 * the edge stays hard.
 *
 * Only ever used for a mesh that arrived with no normals at all. It matters
 * because assimp's default is 175 degrees, which smooths practically everything,
 * and a cube whose eight corners have been smoothed together looks like a
 * lighting bug rather than like a missing attribute. 66 keeps a cube a cube and
 * still smooths anything genuinely curved.
 * */
constexpr float kSmoothingAngleDegrees = 66.0f;

/**
 * The processing assimp is asked for, and just as importantly what it is not.
 *
 * NOT PreTransformVertices: that flattens the hierarchy into one baked mesh,
 * which is the exact opposite of wanting a node graph.
 *
 * NOT FlipUVs: glTF puts the UV origin at the top left and so does Vulkan. The
 * flag exists for OpenGL's bottom-left convention, and using it here would turn
 * every texture upside down.
 *
 * NOT FlipWindingOrder or MakeLeftHanded: glTF is right-handed with
 * counter-clockwise front faces, which is what the pipelines are built for.
 * */
constexpr unsigned int kPostProcessFlags =
        aiProcess_Triangulate |
        // Only fires where normals are missing; a glTF from Blender has them.
        aiProcess_GenSmoothNormals |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        // Splits mixed meshes so a mesh is entirely triangles or entirely not,
        // which is what makes the primitive-type check below a simple equality.
        aiProcess_SortByPType;

glm::vec3 ToGlm(const aiVector3D& v) {
    return glm::vec3(v.x, v.y, v.z);
}

/**
 * One assimp mesh into one of ours, uploaded and handed to the world.
 *
 * Returns null for anything that is not a triangle mesh: points and lines come
 * out of SortByPType as their own meshes and there is no pipeline for them.
 * */
Mesh* BuildMesh(World& world, const aiMesh& src, const std::string& name) {
    if (src.mPrimitiveTypes != aiPrimitiveType_TRIANGLE) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "glTF: skipping '%s', not a triangle mesh (types 0x%x)", name.c_str(),
                            src.mPrimitiveTypes);
        return nullptr;
    }
    if (src.mNumVertices == 0 || src.mNumFaces == 0) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "glTF: skipping empty mesh '%s'",
                            name.c_str());
        return nullptr;
    }
    if (src.HasBones()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "glTF: '%s' has %u bones but skinning is not loaded yet, so it will "
                            "draw rigid, in its bind pose",
                            name.c_str(), src.mNumBones);
    }

    std::vector<StaticVertex> vertices(src.mNumVertices);
    const bool hasUv = src.HasTextureCoords(0);
    for (unsigned int i = 0; i < src.mNumVertices; ++i) {
        vertices[i].position = ToGlm(src.mVertices[i]);
        // Normals are guaranteed by GenSmoothNormals, but a degenerate mesh can
        // still come back without them.
        vertices[i].normal = src.mNormals != nullptr ? ToGlm(src.mNormals[i]) : glm::vec3(0.0f);
        vertices[i].uv = hasUv ? glm::vec2(src.mTextureCoords[0][i].x, src.mTextureCoords[0][i].y)
                               : glm::vec2(0.0f);
    }
    if (!hasUv) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "glTF: '%s' has no UVs, filling with zeros",
                            name.c_str());
    }

    // Triangulate guarantees three per face, so the count is exact and the
    // vector never grows.
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(src.mNumFaces) * 3);
    for (unsigned int f = 0; f < src.mNumFaces; ++f) {
        const aiFace& face = src.mFaces[f];
        if (face.mNumIndices != 3) {
            continue;
        }
        indices.push_back(face.mIndices[0]);
        indices.push_back(face.mIndices[1]);
        indices.push_back(face.mIndices[2]);
    }
    if (indices.empty()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "glTF: '%s' produced no triangles",
                            name.c_str());
        return nullptr;
    }

    MeshDesc desc;
    desc.name = name;
    desc.format = VertexFormat::Static;
    // Immutable: geometry off disk is never rewritten, so it gets one copy
    // rather than one per frame in flight.
    desc.storage = MeshStorage::Immutable;
    desc.vertices = vertices.data();
    desc.vertexCount = static_cast<uint32_t>(vertices.size());
    desc.indices = indices.data();
    desc.indexCount = static_cast<uint32_t>(indices.size());

    std::string error;
    Mesh* mesh = world.AddMesh(Mesh::Create(world.device().allocator().handle(), desc, error));
    if (mesh == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "glTF: %s", error.c_str());
    }
    return mesh;
}

/** Shared state for the recursive walk, so it is not five parameters deep. */
struct Conversion {
    World& world;
    const aiScene& scene;
    GltfLoadResult& result;
    /** assimp mesh index -> our mesh. Null for one that was skipped. */
    std::unordered_map<unsigned int, Mesh*> meshCache;

    Mesh* GetMesh(unsigned int index, const std::string& fallbackName) {
        const auto found = meshCache.find(index);
        if (found != meshCache.end()) {
            return found->second;
        }
        const aiMesh& src = *scene.mMeshes[index];
        const std::string name =
                src.mName.length > 0 ? std::string(src.mName.C_Str()) : fallbackName;
        Mesh* mesh = BuildMesh(world, src, name);
        meshCache.emplace(index, mesh);
        if (mesh != nullptr) {
            result.meshes.push_back(mesh);
        }
        return mesh;
    }

    std::unique_ptr<Node> ConvertNode(const aiNode& src) {
        auto node = Node::Create(src.mName.length > 0 ? std::string(src.mName.C_Str())
                                                      : std::string("node"));
        Node* raw = node.get();
        result.nodes.push_back(raw);

        // Decompose rather than store the matrix: a Node's transform IS position,
        // rotation and scale, and everything downstream (a behaviour that spins
        // the cube, the Euler view) edits those. Decompose reads assimp's
        // row-major matrix correctly on its own, so there is no transpose here;
        // one would silently produce a transform that is almost right.
        aiVector3D scale;
        aiQuaternion rotation;
        aiVector3D position;
        src.mTransformation.Decompose(scale, rotation, position);
        raw->position = ToGlm(position);
        raw->scale = ToGlm(scale);
        // glm::quat takes w first; aiQuaternion stores it first too, but the
        // constructor argument order is the thing that catches people out.
        raw->SetRotation(glm::quat(rotation.w, rotation.x, rotation.y, rotation.z));

        for (unsigned int i = 0; i < src.mNumMeshes; ++i) {
            Mesh* mesh = GetMesh(src.mMeshes[i], raw->name);
            if (mesh == nullptr) {
                continue;
            }
            if (i == 0) {
                raw->renderable.emplace(*mesh);
            } else {
                // A Renderable is one per Node, so the extra primitives of a
                // multi-material mesh become children. They inherit the parent's
                // transform by sitting under it, which is exactly right: they
                // are the same object, cut up by material.
                auto extra = Node::Create(raw->name + "_prim" + std::to_string(i));
                extra->renderable.emplace(*mesh);
                Node* extraRaw = raw->AddChild(std::move(extra));
                result.nodes.push_back(extraRaw);
            }
        }

        for (unsigned int i = 0; i < src.mNumChildren; ++i) {
            raw->AddChild(ConvertNode(*src.mChildren[i]));
        }
        return node;
    }
};

} // namespace

bool LoadGltf(World& world, const std::string& assetPath, GltfLoadResult& result,
              std::string& error) {
    const std::vector<uint8_t> bytes = assets::Read(assetPath, error);
    if (bytes.empty()) {
        return false;
    }

    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, kSmoothingAngleDegrees);

    // From memory, not from a path: an asset inside an APK is not a file, so
    // there is nothing for assimp's default IO system to open. The "glb" hint
    // tells it which importer to use without a filename to guess from, and it
    // is also why only .glb works, since a plain .gltf would send the parser
    // looking for sibling .bin and image files that do not exist here.
    const aiScene* scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(),
                                                       kPostProcessFlags, "glb");
    if (scene == nullptr || scene->mRootNode == nullptr) {
        error = "assimp could not read '" + assetPath + "': " + importer.GetErrorString();
        return false;
    }
    if ((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        // Not fatal (it usually means something this pass ignores anyway) but
        // it is the first thing to look at when geometry is missing.
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "glTF: '%s' loaded incomplete",
                            assetPath.c_str());
    }

    Conversion conversion{world, *scene, result, {}};

    // assimp wraps the file in a root of its own, so the file's real top-level
    // nodes are usually that root's children and dropping the wrapper keeps an
    // extra identity node out of every loaded scene.
    //
    // Usually. A root that carries geometry itself is a real node, not a
    // wrapper, and unwrapping it would throw the model away, so in that case,
    // and in the degenerate case of a root with no children at all, it is kept.
    const aiNode& root = *scene->mRootNode;
    if (root.mNumMeshes > 0 || root.mNumChildren == 0) {
        result.roots.push_back(conversion.ConvertNode(root));
    } else {
        result.roots.reserve(root.mNumChildren);
        for (unsigned int i = 0; i < root.mNumChildren; ++i) {
            result.roots.push_back(conversion.ConvertNode(*root.mChildren[i]));
        }
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "glTF: '%s' -> %zu roots, %zu nodes, %zu meshes", assetPath.c_str(),
                        result.roots.size(), result.nodes.size(), result.meshes.size());
    return true;
}

} // namespace putorana::graphics
