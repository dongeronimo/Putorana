#version 450

// First mesh vertex shader. The set layout here is the contract in Material.h
// and it is the same for every shader that draws geometry:
//
//   set 0 = frame globals   (camera; lights when there are any)  -- the pass owns it
//   set 1 = per-object data indexed by gl_InstanceIndex          -- the pass owns it
//   set 2 = this material's parameters and textures              -- the material owns it
//
// Locations match VertexInputFor() in Mesh.h, and they are shared by the static
// and the skinned formats, so this shader reads a skinned mesh correctly too and
// simply ignores locations 3 and 4.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

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
layout(location = 1) out vec2 outUv;

void main() {
    // gl_InstanceIndex is firstInstance plus the instance counter, and the pass
    // fills the object buffer in draw order with firstInstance pointing at each
    // run's first slot. That identity is the whole instancing scheme.
    ObjectData object = objects[gl_InstanceIndex];

    gl_Position = frame.projection * frame.view * object.model * vec4(inPosition, 1.0);

    // The inverse transpose, not the model matrix: under non-uniform scale the
    // model matrix skews a normal off the surface it belongs to.
    outNormal = mat3(object.normalMatrix) * inNormal;
    outUv = inUv;
}
