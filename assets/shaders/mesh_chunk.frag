#version 450

// The reconstruction's fragment shader: the colour the camera saw at each voxel,
// falling back to a flat lit colour wherever there is no camera colour yet.
//
// See mesh_chunk.vert for why this is a separate shader rather than a reuse of
// mesh_flat.frag.

layout(location = 0) in vec3 inNormal;

// .rgb is gamma-encoded camera colour, .a is how much evidence stands behind it,
// NOT opacity. See ReconstructedVertex in Mesh.h.
layout(location = 1) in vec4 inColor;

layout(set = 2, binding = 0, std140) uniform MaterialParams {
    vec4 color;
    // How far to trust inColor.rgb at all. Multiplied by the per-vertex
    // confidence, so 0 draws the whole reconstruction as it looked before there
    // was colour. See ChunkMaterial::SetColorMix for why that setting exists.
    float colorMix;
    // True when the attachment is an _SRGB format, in which case the hardware
    // encodes on write and this shader has to hand it linear values. Same flag,
    // same meaning and the same source as composite.frag's: the feed and the
    // geometry drawn over it have to agree.
    int srgbTarget;
} material;

layout(location = 0) out vec4 outColor;

const vec3 kLightDirection = normalize(vec3(0.4, 0.9, 0.5));
// Not zero: an unlit face should read as dark, not as a hole.
const float kAmbient = 0.25;

// How much the directional light is allowed to touch CAMERA colour.
//
// Small, and the smallness is the whole point. Camera colour is not albedo, it is
// radiance: it already contains this room's real lighting, its real shadows and
// its real bounce. Shading it again with a fictional sun double-darkens every
// surface that happens to face away from a direction that means nothing in the
// scene, which reads as a reconstruction with dirt on it.
//
// It is not zero either, because a fully unlit surface loses its silhouette
// against another surface of the same colour, and one of the things this view has
// to keep answering is whether the geometry is right. A tenth is enough to see
// the shape and too little to look like lighting.
const float kColorShading = 0.1;

// The sRGB EOTF, exactly rather than the pow(2.2) approximation, and identical to
// the one in composite.frag by necessity rather than by copy-paste laziness.
//
// The camera feed goes through that function on its way to the screen, and this
// geometry is drawn directly in front of the feed it was sampled from. Any
// disagreement between the two shows up as reconstructed surfaces that are
// visibly a different colour from the pixels immediately around them, which reads
// as an integration bug and is not one.
vec3 SrgbToLinear(vec3 c) {
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    vec3 low = c / 12.92;
    return mix(low, high, step(vec3(0.04045), c));
}

void main() {
    // Normalised here rather than in the vertex shader because interpolating
    // unit vectors across a triangle does not preserve their length.
    vec3 normal = normalize(inNormal);
    float diffuse = max(dot(normal, kLightDirection), 0.0);

    // Where there is no camera colour: the flat material colour, fully lit, which
    // is exactly what this shader drew before colour existed.
    vec3 flat_ = material.color.rgb * (kAmbient + (1.0 - kAmbient) * diffuse);

    // Where there is: the camera colour, barely lit per kColorShading.
    //
    // Linearised only for an _SRGB attachment, where the hardware re-encodes on
    // write. On a _UNORM one nothing re-encodes, composite.frag leaves the feed
    // gamma-encoded too, and linearising here would make the reconstruction
    // visibly darker than the pixels around it.
    vec3 cameraRgb = material.srgbTarget != 0 ? SrgbToLinear(inColor.rgb) : inColor.rgb;
    vec3 camera = cameraRgb * (1.0 - kColorShading + kColorShading * diffuse);

    // The per-vertex confidence times the material's global trust. Both are
    // needed: the first says where colour has converged, the second is the
    // switch that turns the whole thing off.
    float trust = inColor.a * material.colorMix;

    outColor = vec4(mix(flat_, camera, trust), material.color.a);
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
//
// And a second note, for the first time it renders in the WRONG COLOUR, because
// the tempting fix is in this file too and it is the wrong one. Three things
// upstream of here decide the colour, in the order worth checking:
//
//   1. the depth-to-colour affine map, which RECONPROBE prints once on the first
//      frame with an image. Its two scales must be equal. Colour that slides off
//      the geometry toward the edges of frame is this.
//   2. the YUV conversion in YuvColorImage.h, which must match CameraRgb() in
//      composite.frag coefficient for coefficient. Colour that is uniformly
//      washed out, or has red and blue swapped, is this.
//   3. SrgbToLinear above. Colour that is right in hue but too dark or too
//      bright against the feed behind it is this.
//
// Multiplying a correction into outColor fixes the symptom at whichever of the
// three is broken and leaves the other two to disagree later.
