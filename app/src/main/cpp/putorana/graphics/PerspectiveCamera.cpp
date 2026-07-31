#include "PerspectiveCamera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace putorana::graphics {

std::unique_ptr<PerspectiveCamera> PerspectiveCamera::Create() {
    return std::unique_ptr<PerspectiveCamera>(new PerspectiveCamera());
}

glm::mat4 PerspectiveCamera::ProjectionMatrix(VkExtent2D framebufferExtent,
                                              VkSurfaceTransformFlagBitsKHR preTransform) const {
    // ---- 1. aspect ratio, in the orientation the USER sees ----
    //
    // The swapchain extent is in the presentation engine's natural orientation.
    // Under a 90 or 270 degree rotation the image the user is looking at is the
    // transpose of that, so the camera has to frame a scene of extent.height by
    // extent.width. Get this backwards and everything is stretched, in a way
    // that looks like a bad FOV rather than like a rotation bug.
    const bool axesSwapped = SurfaceAxesSwapped(preTransform);
    const float width =
            static_cast<float>(axesSwapped ? framebufferExtent.height : framebufferExtent.width);
    const float height =
            static_cast<float>(axesSwapped ? framebufferExtent.width : framebufferExtent.height);
    if (width <= 0.0f || height <= 0.0f) {
        // A window mid-resize legitimately has no area for a frame. The caller
        // skips the frame anyway; returning identity keeps this from producing a
        // matrix full of NaN that would poison a buffer.
        return glm::mat4(1.0f);
    }

    // ---- 2. the projection itself ----
    //
    // RH: right-handed, camera looking down -Z. That is the glTF convention and
    // the one the whole engine uses (Light's direction, a future Node::LookAt).
    //
    // ZO: clip Z in [0,1]. Vulkan's depth range, and also WebGPU's — but NOT
    // GLM's default, which is OpenGL's [-1,1]. GLM only switches globally with
    // GLM_FORCE_DEPTH_ZERO_TO_ONE, and a global that silently halves everyone's
    // depth precision if someone drops it is worse than naming the variant here.
    glm::mat4 projection =
            glm::perspectiveRH_ZO(glm::radians(fovY), width / height, nearPlane, farPlane);

    // ---- 3. Vulkan's Y points DOWN ----
    //
    // In OpenGL and WebGPU, NDC +Y is the top of the image. In Vulkan it is the
    // bottom. GLM builds the former, so one negated term turns it into the
    // latter. Without this the scene renders upside down — and, more insidiously,
    // triangle winding reverses, so back-face culling quietly removes exactly the
    // faces it should keep.
    projection[1][1] *= -1.0f;

    // ---- 4. pre-rotation ----
    //
    // See SurfaceRotation in Camera.h. It multiplies from the LEFT because it
    // acts on clip space, after everything else: rotate * proj * view * model.
    // And it comes after the Y flip, so the rotation happens in Vulkan's y-down
    // clip space, where a positive angle reads as clockwise on screen — which is
    // what ROTATE_90 means in the spec.
    return SurfaceRotation(preTransform) * projection;
}

} // namespace putorana::graphics
