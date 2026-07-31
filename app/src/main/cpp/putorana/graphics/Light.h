#ifndef PUTORANA_GRAPHICS_LIGHT_H
#define PUTORANA_GRAPHICS_LIGHT_H

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace putorana::graphics {

/**
 * Which kind of light this is, as a plain field rather than a virtual call.
 *
 * The thing that consumes lights buckets them by type and writes three
 * differently shaped arrays to the GPU — one switch at collection time, then a
 * static_cast. That is a data-oriented job, so paying for virtual dispatch to
 * ask "what are you" would be backwards.
 * */
enum class LightType {
    Point,
    Spot,
    Directional,
};

/**
 * A light source, as scene data. Hangs off a Node the same way a Renderable
 * does, and knows nothing about Vulkan — no buffer, no descriptor, not even an
 * include. Whatever gathers lights each frame reads these fields and packs them.
 *
 * ## What is deliberately NOT here: position and direction
 *
 * Both come from the owning node's world matrix. Position is its translation;
 * direction is its -Z axis, which is the same convention the camera uses ("the
 * camera looks down its node's -Z") and the same one glTF uses.
 *
 * Storing a direction here instead would duplicate the node's rotation in a
 * second place, and two copies of a rotation drift. It would also mean a
 * spotlight could not simply be parented to a torch and follow it.
 *
 * ## Units
 *
 * Colour is linear RGB, not sRGB. It gets multiplied by intensity and summed
 * with other lights, and both of those are only meaningful in linear space —
 * the conversion to sRGB happens once, at the very end of the frame.
 * */
class Light {
public:
    virtual ~Light();

    LightType type() const { return type_; }

    /** Linear RGB in [0,1]. Multiplied by intensity when the light is composed. */
    glm::vec3 color{1.0f, 1.0f, 1.0f};

    /**
     * How strong the light is.
     *
     * Point and Spot: the numerator of the linear falloff intensity/distance.
     * Directional: a plain multiplier, since a light infinitely far away does
     * not attenuate.
     * */
    float intensity = 1.0f;

    /**
     * May this light be dropped when it falls outside the camera frustum?
     *
     * The default is yes, which is right for a torch or a muzzle flash. A light
     * meant to illuminate the whole scene — a sun — has to set this false, or it
     * will blink out the moment it leaves the frame. Directional lights have no
     * position to test in the first place, so for them the flag never comes up.
     *
     * Nothing reads it yet; the culling that would is part of the pass that
     * gathers lights, which does not exist.
     * */
    bool cullable = true;

protected:
    explicit Light(LightType type) : type_(type) {}

    // Protected rather than public, which is the standard recipe for a
    // polymorphic base: subclasses stay freely copyable, but assigning a
    // SpotLight to a Light& — which would slice the cone angles off and leave a
    // half-copied object behind — will not compile.
    Light(const Light&) = default;
    Light& operator=(const Light&) = default;

private:
    LightType type_;
};

/** Radiates in every direction from the node's world position. */
class PointLight : public Light {
public:
    PointLight() : Light(LightType::Point) {}
};

/**
 * A cone from the node's world position, pointing down the node's -Z.
 *
 * Angles are in RADIANS, measured from the cone's centre axis — not degrees,
 * unlike Camera::fovY. The inconsistency is deliberate and comes from where the
 * numbers originate: these are read verbatim out of a glTF's KHR_lights_punctual,
 * which specifies radians, while a field of view is a number a human types.
 * Converting either one at the boundary would just move the conversion to a
 * place with fewer eyes on it.
 * */
class SpotLight : public Light {
public:
    SpotLight() : Light(LightType::Spot) {}

    /** Full brightness out to here. */
    float innerConeAngle = glm::pi<float>() / 8.0f;
    /** Falls to zero here. Must be >= innerConeAngle. */
    float outerConeAngle = glm::pi<float>() / 6.0f;
};

/**
 * Parallel rays from infinitely far away — a sun. Only the node's -Z matters;
 * its position is ignored entirely, so moving a directional light does nothing.
 * */
class DirectionalLight : public Light {
public:
    DirectionalLight() : Light(LightType::Directional) {}
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_LIGHT_H
