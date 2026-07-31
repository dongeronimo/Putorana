#ifndef PUTORANA_GRAPHICS_ARCAMERA_H
#define PUTORANA_GRAPHICS_ARCAMERA_H

#include "Camera.h"
#include "putorana/ar/Subsystem.h"

#include <memory>

namespace putorana::graphics {

class Node;

/**
 * A camera whose projection comes from ARCore rather than from a field of view.
 *
 * ## Why it cannot just be a PerspectiveCamera with the right FOV
 *
 * The projection has to match the physical sensor that took the picture behind
 * it — its focal length, its principal point, the crop ARCore applied to fit the
 * viewport. Getting that from a fovY and an aspect ratio means reproducing
 * ARCore's own arithmetic and agreeing with it to the last decimal. Where it
 * disagrees, virtual objects do not look mis-framed; they look like they are
 * sliding over the world instead of sitting in it, and only while the phone
 * moves. So the matrix is taken, not computed.
 *
 * ## The pose goes into the node, not into this class
 *
 * SetFrame writes ARCore's position and attitude onto the owning Node's
 * transform. Two things fall out of that, both wanted:
 *
 *   - the view matrix needs no special case. MeshPass already builds it as the
 *     inverse of the camera node's world matrix, and ARCore's view matrix is by
 *     definition the inverse of the pose that was just written there;
 *   - the scene graph tells the truth. Anything that wants to know where the
 *     device is, or what it is pointing at, reads the node like it would read
 *     any other — including anything parented to it later.
 *
 * ## Conventions
 *
 * ARCore's C API is OpenGL-shaped throughout: +Y up, clip Z in [-1,1], column
 * major. Vulkan wants +Y down and Z in [0,1]. Both conversions happen here, once
 * per frame, rather than being left for a shader to guess at.
 * */
class ArCamera : public Camera {
public:
    static std::unique_ptr<ArCamera> Create();

    /**
     * Near and far clip planes, in METRES — ARCore works in real-world units, so
     * these are not arbitrary the way a synthetic scene's are.
     *
     * Push them to the subsystem with ar::Subsystem::SetClipPlanes; they are the
     * inputs it builds CameraFrame::projection from, and changing them here
     * alone does nothing.
     * */
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;

    /**
     * Takes this frame's ARCore output: caches the projection, and writes the
     * pose onto `node`.
     *
     * Call once per frame, from the world's Update, BEFORE the base
     * World::Update closes the world matrices — otherwise the camera's transform
     * lands a frame late and every virtual object lags the picture by one frame.
     *
     * Ignores a frame whose tracking state is not TRACKING: the pose is stale
     * rather than approximately right, and following it would throw the scene
     * across the room. The projection is still taken, because it depends on the
     * display geometry rather than on tracking.
     * */
    void SetFrame(const ar::CameraFrame& frame, Node& node);

    /** True once a tracking frame has been seen. Until then the pose is a guess. */
    bool tracking() const { return tracking_; }

    glm::mat4 ProjectionMatrix(VkExtent2D framebufferExtent,
                               VkSurfaceTransformFlagBitsKHR preTransform) const override;

private:
    ArCamera() = default;

    /** Already converted to Vulkan conventions. Identity until the first SetFrame. */
    glm::mat4 projection_{1.0f};
    bool tracking_ = false;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_ARCAMERA_H
