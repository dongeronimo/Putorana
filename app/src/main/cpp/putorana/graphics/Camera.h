#ifndef PUTORANA_GRAPHICS_CAMERA_H
#define PUTORANA_GRAPHICS_CAMERA_H

#include "volk.h"

#include <glm/glm.hpp>

namespace putorana::graphics {

/**
 * What a node needs to be a camera: a way to produce a projection matrix.
 *
 * Abstract, because there are two genuinely different sources for one. A
 * PerspectiveCamera computes it from a field of view and an aspect ratio, the
 * way every renderer does. An ArCamera does not compute it at all: it is handed
 * one by ARCore, built from the physical sensor's intrinsics, and inventing a
 * projection instead would put the virtual geometry in a subtly different frustum
 * from the camera image behind it. That mismatch is not visible as a wrong FOV;
 * it is visible as objects that refuse to stay stuck to the world.
 *
 * ## What is NOT here: the view matrix
 *
 * It comes from the owning node, the inverse of that node's world matrix,
 * because a camera looks down its own -Z. Same convention as Light's direction,
 * same as glTF's. Keeping the view out of this class is what lets a camera be
 * parented to a ship and follow it with no code at all, and it is also what lets
 * an ArCamera work without a special case: the AR pose is written INTO the node,
 * so the inverse of the world matrix is already ARCore's view matrix.
 *
 * ## Ownership
 *
 * A Node holds this by unique_ptr, not by optional. Polymorphism means a pointer:
 * an optional<Camera> stores a Camera by value and would slice an ArCamera
 * down to its base. Same reasoning as Light, and the same shape.
 * */
class Camera {
public:
    virtual ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    /**
     * World -> clip, ready for the shader to use as `proj * view * model`.
     *
     * @param framebufferExtent  what is being rendered into, in the swapchain's
     *                           own orientation (Swapchain::extent()).
     * @param preTransform       Swapchain::preTransform(). The swapchain is
     *                           created with the surface's currentTransform, so
     *                           rotating the image is this renderer's job; see
     *                           SurfaceRotation.
     * */
    virtual glm::mat4 ProjectionMatrix(VkExtent2D framebufferExtent,
                                       VkSurfaceTransformFlagBitsKHR preTransform) const = 0;

protected:
    Camera() = default;
};

/**
 * Rotation about the clip-space Z axis matching a surface transform.
 *
 * The swapchain is created with preTransform = the surface's currentTransform
 * (see Swapchain.h), which is a promise to the presentation engine that the
 * image content arrives already rotated. That saves the compositor a full screen
 * blit on every single frame, which is the whole reason to do it, and the price
 * is that fulfilling the promise is the renderer's job.
 *
 * Every projection matrix in this renderer is multiplied by this from the LEFT,
 * because it acts on clip space, after everything else. CameraFeed applies the
 * same rotation to its texture coordinates, for the same reason and in the same
 * direction, and those two have to agree or the virtual content and the camera
 * image end up rotated relative to each other.
 *
 * Built by hand from exact integers rather than with glm::rotate, because
 * cos(pi/2) in floating point is -4.4e-8 and not zero. These four matrices are
 * permutations of the axes and deserve to be exactly that: a projection that is
 * a hair off square is a needle in a haystack later.
 *
 * Only the four rotations are handled. The mirrored transforms exist in the spec
 * but no Android compositor reports one, and INHERIT means the platform is not
 * telling; identity is the honest answer to all of them.
 * */
glm::mat4 SurfaceRotation(VkSurfaceTransformFlagBitsKHR transform);

/** True for the transforms under which the user sees the extent transposed. */
bool SurfaceAxesSwapped(VkSurfaceTransformFlagBitsKHR transform);

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_CAMERA_H
