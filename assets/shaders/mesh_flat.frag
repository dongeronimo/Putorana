#version 450

// A solid colour with one hardcoded light direction.
//
// The renderer this is ported from starts with a genuinely unlit material —
// colour straight out, nothing else — as the template for the ones that follow.
// This one adds four lines of diffuse on purpose: a rotating cube shaded in flat
// colour is a hexagon, and being able to see that geometry is arriving with the
// right winding, the right normals and the right depth order is the entire point
// of the first thing that draws.
//
// The direction is baked in because Light exists but nothing collects lights
// yet. When set 0 grows a light array this becomes the first real material, and
// the change is confined to this file.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUv;

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
