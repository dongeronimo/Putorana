#version 450

// The final pass: puts the virtual world onto the real one and writes the
// result to the swapchain.
//
// Two jobs in one fullscreen triangle, because both of them need the mesh pass's
// attachment and doing them in separate passes would mean storing it and loading
// it back — a full screen write plus a full screen read on a tiler, for nothing:
//
//   1. turn the camera's YUV_420_888 planes into RGB;
//   2. composite the mesh pass's output over that, by its alpha.
//
// The mesh pass clears its target to TRANSPARENT black when there is a camera
// image, so alpha is 0 wherever no geometry was drawn and the feed shows
// through. With no camera image it clears opaquely instead, alpha is 1
// everywhere, and the mix below picks the mesh target throughout — the same
// picture this pass produced before there was any AR in it, and no branch here
// to arrange it.
//
// This is still where tone mapping goes, the day the mesh pass draws into a
// float target. Note the order it will have to take: the mesh contribution is
// scene-referred and must be tone mapped, while the camera feed is already
// display-referred and must NOT be. So it belongs above the mix, on meshColor
// alone.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec2 inCameraUv;

layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(set = 0, binding = 1) uniform sampler2D lumaPlane;
layout(set = 0, binding = 2) uniform sampler2D chromaPlane;

layout(push_constant) uniform CompositePush {
    vec2 cameraUv0;
    vec2 cameraUv1;
    vec2 cameraUv2;
    // NV21 (V in the first byte of each pair) rather than NV12. Both occur, so
    // the CPU side detects it from the plane pointers and says which.
    int vFirst;
    // True when the attachment is an _SRGB format, in which case the hardware
    // encodes on write and this shader has to hand it linear values.
    int srgbTarget;
} push;

layout(location = 0) out vec4 outColor;

// The sRGB EOTF, exactly rather than the pow(2.2) approximation. The piecewise
// linear segment near black is the part the approximation gets wrong, and dark
// is where a camera feed spends much of its range indoors.
vec3 SrgbToLinear(vec3 c) {
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    vec3 low = c / 12.92;
    return mix(low, high, step(vec3(0.04045), c));
}

vec3 CameraRgb() {
    // BT.601, studio swing: luma occupies 16..235 and chroma 16..240, which is
    // what a camera pipeline produces. Treating it as full range is the classic
    // mistake and shows up as milky blacks and clipped highlights.
    float y = (texture(lumaPlane, inCameraUv).r - 0.0625) * 1.164383;

    vec2 chroma = texture(chromaPlane, inCameraUv).rg;
    float u = (push.vFirst != 0 ? chroma.g : chroma.r) - 0.5;
    float v = (push.vFirst != 0 ? chroma.r : chroma.g) - 0.5;

    vec3 rgb = clamp(vec3(y + 1.596027 * v,
                          y - 0.391762 * u - 0.812968 * v,
                          y + 2.017232 * u),
                     0.0, 1.0);

    // What comes out of that matrix is gamma-encoded — display-ready values, the
    // same encoding a JPEG carries. The mix below happens in linear light and the
    // swapchain re-encodes on write, so it has to be linearised here. Mixing
    // gamma-encoded values against linear ones is the kind of bug that looks like
    // a wrong blend factor.
    return push.srgbTarget != 0 ? SrgbToLinear(rgb) : rgb;
}

void main() {
    // Sampling an _SRGB image decodes to linear in hardware. Alpha is never
    // sRGB-encoded, by spec, so it arrives untouched — which is what makes it
    // usable directly as the mix factor.
    vec4 meshColor = texture(sourceImage, inUv);

    outColor = vec4(mix(CameraRgb(), meshColor.rgb, meshColor.a), 1.0);
}
