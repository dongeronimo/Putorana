#version 450

// The final pass: copies what the mesh pass drew onto the swapchain image.
//
// Straight through for now. This is where tone mapping goes the day the mesh
// pass draws into a float target and light is allowed past 1.0 — which is the
// reason this pass exists at all rather than the mesh pass drawing into the
// swapchain image directly.

layout(location = 0) in vec2 inUv;

layout(set = 0, binding = 0) uniform sampler2D sourceImage;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(sourceImage, inUv);
}
