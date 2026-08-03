#version 450

// The reconstruction's vertex shader. Identical to mesh_flat.vert except that it
// reads VertexFormat::PositionNormal -- 24 bytes, no UV.
//
// Why a second shader rather than reusing mesh_flat: vertex input layout is baked
// into a pipeline, so a format without location 2 needs its own. Reusing
// mesh_flat against a 24-byte vertex would have the pipeline fetch 8 bytes past
// the end of the last vertex in the buffer.
//
// The duplication against mesh_flat.vert is deliberate and known. Chunk meshes
// are generated geometry with no texture, so a UV lane would be 8 bytes of
// zeroes per vertex that nothing reads -- on the order of 8 MB for a room at 4cm
// voxels, plus its share of every byte re-uploaded on every remesh. See the
// comment on VertexFormat::PositionNormal in Mesh.h.
//
// The set layout is the contract in Material.h and is the same for every shader
// that draws geometry:
//
//   set 0 = frame globals   (camera; lights when there are any)  -- the pass owns it
//   set 1 = per-object data indexed by gl_InstanceIndex          -- the pass owns it
//   set 2 = this material's parameters and textures              -- the material owns it

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// std140 to match the C++ FrameData: a uniform block's default packing is not
// specified for Vulkan, so saying which one is not optional.
layout(set = 0, binding = 0, std140) uniform Frame {
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
} frame;

struct ObjectData {
    mat4 model;
    mat4 normalMatrix;
};

// std430 to match the C++ ObjectData. Two mat4 pack identically under 140 and
// 430, but the array stride does not: std140 would round each element up to a
// multiple of 16 bytes, which for this struct is a no-op and for the next one
// might not be.
layout(set = 1, binding = 0, std430) readonly buffer Objects {
    ObjectData objects[];
};

layout(location = 0) out vec3 outNormal;

void main() {
    // gl_InstanceIndex is firstInstance plus the instance counter, and the pass
    // fills the object buffer in draw order with firstInstance pointing at each
    // run's first slot. That identity is the whole instancing scheme.
    ObjectData object = objects[gl_InstanceIndex];

    // The model matrix matters more here than it does for an authored mesh.
    // Chunk vertices are CHUNK LOCAL -- the origin is subtracted on the way out
    // of the reconstruction -- so the node's transform is what puts the chunk
    // back in the world. It keeps the floats small near the origin instead of
    // tens of metres out, where a float32 mantissa starts costing sub-millimetre
    // detail that marching cubes worked to produce.
    gl_Position = frame.projection * frame.view * object.model * vec4(inPosition, 1.0);

    // The inverse transpose, not the model matrix: under non-uniform scale the
    // model matrix skews a normal off the surface it belongs to.
    outNormal = mat3(object.normalMatrix) * inNormal;
}
