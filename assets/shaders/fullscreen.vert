#version 450

// The final pass's vertex shader: one triangle that covers the screen, generated
// from gl_VertexIndex with no vertex buffer, no index buffer and no draw state
// beyond vkCmdDraw(cmd, 3, 1, 0, 0).
//
// A triangle and not a quad, and it is not a micro-optimisation. A quad is two
// triangles meeting along the diagonal, and the GPU rasterises in 2x2 quads of
// pixels: every quad straddling that seam gets shaded twice. One oversized
// triangle clipped to the viewport has no seam.
//
//   index 0 -> (-1,-1) uv (0,0)
//   index 1 -> ( 3,-1) uv (2,0)
//   index 2 -> (-1, 3) uv (0,2)
//
// The corner off the screen is what makes the visible part cover [-1,1] exactly.
//
// Note the uv derivation assumes Vulkan's clip space, where -1 on Y is the TOP
// of the image. So uv.y = (position.y + 1) / 2 maps the top of the screen to
// v = 0, which is where glTF and Vulkan both put the texture origin.

layout(push_constant) uniform CompositePush {
    // Where the same three corners land in the CAMERA image. These cannot be
    // derived here the way the mesh UVs can: the sensor has a fixed orientation
    // and a fixed aspect ratio, neither of which is the viewport's, and the
    // mapping between them changes on every rotation. ARCore owns that mapping
    // and hands back the corners already transformed.
    vec2 cameraUv0;
    vec2 cameraUv1;
    vec2 cameraUv2;
    // Read by the fragment stage. Declared in one block visible to both stages
    // rather than split into two ranges: push constant ranges that overlap in
    // memory but differ in stage flags are legal and are a reliable way to spend
    // an afternoon on a validation error.
    int vFirst;
    int srgbTarget;
} push;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec2 outCameraUv;

void main() {
    outUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUv * 2.0 - 1.0, 0.0, 1.0);

    vec2 cameraUvs[3] = vec2[3](push.cameraUv0, push.cameraUv1, push.cameraUv2);
    outCameraUv = cameraUvs[gl_VertexIndex];
}
