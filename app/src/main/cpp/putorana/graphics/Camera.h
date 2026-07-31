#ifndef PUTORANA_GRAPHICS_CAMERA_H
#define PUTORANA_GRAPHICS_CAMERA_H

#include "volk.h"

#include <glm/glm.hpp>

namespace putorana::graphics {

/**
 * The projection half of a camera. Hangs off a Node like a Renderable does.
 *
 * ## What is NOT here: the view matrix
 *
 * It comes from the owning node — it is the inverse of that node's world matrix,
 * because the camera looks down its node's -Z. Same convention as Light's
 * direction, same as glTF's. Keeping the view out of this class is what lets a
 * camera be parented to a ship and follow it with no code at all.
 *
 * ## Why this is not a 29-line class like the WebGPU original
 *
 * Three transforms sit between a right-handed world and an Android screen, and
 * none of them are optional. ProjectionMatrix applies all three; the comments on
 * its implementation say what each one is for.
 * */
class Camera {
public:
    /**
     * Vertical field of view, in DEGREES.
     *
     * Degrees because this is a number a human types, and the node's Euler
     * angles are in degrees for the same reason. (Spotlight cone angles are in
     * radians because those are read verbatim out of a glTF — see Light.h.)
     * */
    float fovY = 60.0f;

    /**
     * Near and far clip planes. nearPlane must be > 0 — a perspective projection
     * divides by it.
     *
     * Named with the suffix because `near` and `far` are macros in <windef.h>,
     * and while nothing here includes it, a member you cannot name in an IDE's
     * analysis pass is a bad trade for four saved characters.
     *
     * Depth precision lives almost entirely in the ratio far/nearPlane, not in
     * the absolute values: pushing nearPlane from 0.1 to 0.5 buys more precision
     * than pulling farPlane from 1000 to 200.
     * */
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;

    /**
     * World -> clip, ready for the shader to use as `proj * view * model`.
     *
     * There is no `aspect` field to set, on purpose. The aspect ratio is derived
     * from the extent here because under a 90 or 270 degree surface rotation the
     * width and height a camera should reason about are swapped relative to the
     * swapchain's — and a settable field is an invitation to compute it from
     * extent.width/extent.height somewhere else and get exactly that wrong.
     *
     * @param framebufferExtent  what is being rendered into, in the swapchain's
     *                           own orientation (Swapchain::extent()).
     * @param preTransform       Swapchain::preTransform(). The swapchain is
     *                           created with the surface's currentTransform, so
     *                           rotating the image is this renderer's job — see
     *                           the implementation.
     * */
    glm::mat4 ProjectionMatrix(VkExtent2D framebufferExtent,
                               VkSurfaceTransformFlagBitsKHR preTransform) const;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_CAMERA_H
