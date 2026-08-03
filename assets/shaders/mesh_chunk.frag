#version 450

// The reconstruction's fragment shader. Same lighting as mesh_flat.frag, minus
// the UV it has no use for -- see mesh_chunk.vert for why this is a separate
// shader rather than a reuse.
//
// Kept deliberately close to mesh_flat.frag rather than being made clever. The
// first thing a reconstruction has to answer is whether the geometry is arriving
// with the right winding, the right normals and the right depth order, and a
// single directional light answers exactly that. A surface that is noisy, or
// inside out, or facing the wrong way is obvious under this and invisible under
// flat colour.

layout(location = 0) in vec3 inNormal;

layout(set = 2, binding = 0, std140) uniform MaterialParams {
    vec4 color;
} material;

layout(location = 0) out vec4 outColor;

const vec3 kLightDirection = normalize(vec3(0.4, 0.9, 0.5));
// Not zero: an unlit face should read as dark, not as a hole.
const float kAmbient = 0.25;

void main() {
    // Normalised here rather than in the vertex shader because interpolating
    // unit vectors across a triangle does not preserve their length.
    vec3 normal = normalize(inNormal);
    float diffuse = max(dot(normal, kLightDirection), 0.0);
    outColor = vec4(material.color.rgb * (kAmbient + (1.0 - kAmbient) * diffuse),
                    material.color.a);
}

// A note for the first time this renders nothing, because the wrong fix is
// tempting and it is in this file.
//
// Marching cubes emits a CONSISTENT winding: the tables orient every triangle by
// the sign of the field, so normals point uniformly from occupied space toward
// free space. And OpenChisel refuses to emit geometry it cannot trust --
// ChunkManager.cpp only calls MeshCube when allNeighborsObserved, so an
// unobserved region produces no triangles rather than badly oriented ones.
//
// So if the reconstruction is invisible, it is not a lighting problem and no
// change here can fix it. The pipeline culls back faces
// (VK_CULL_MODE_BACK_BIT, counter-clockwise front), which means a sign
// convention opposite to the renderer's makes the whole surface vanish rather
// than turn black. Set cullMode to NONE once to tell "no geometry was generated"
// apart from "the geometry is inside out"; if it appears, the fix is the winding
// or frontFace, not this shader.
