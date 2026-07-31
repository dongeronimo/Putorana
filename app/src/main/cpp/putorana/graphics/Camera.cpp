#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>

namespace putorana::graphics {

namespace {

/**
 * Rotation about the clip-space Z axis matching a surface transform.
 *
 * Built by hand from exact integers rather than with glm::rotate, because
 * cos(pi/2) in floating point is -4.4e-8 and not zero. These four matrices are
 * permutations of the axes and deserve to be exactly that — a projection that is
 * a hair off square is a needle in a haystack later.
 *
 * Only the four rotations are handled. The mirrored transforms exist in the spec
 * but no Android compositor reports one, and INHERIT means the platform is not
 * telling; identity is the honest answer to all of them.
 * */
glm::mat4 SurfaceRotation(VkSurfaceTransformFlagBitsKHR transform) {
    float cosine = 1.0f;
    float sine = 0.0f;
    switch (transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            cosine = 0.0f;
            sine = 1.0f;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            cosine = -1.0f;
            sine = 0.0f;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            cosine = 0.0f;
            sine = -1.0f;
            break;
        default:
            break;
    }

    glm::mat4 rotation(1.0f);
    rotation[0][0] = cosine;
    rotation[0][1] = sine;
    rotation[1][0] = -sine;
    rotation[1][1] = cosine;
    return rotation;
}

} // namespace

glm::mat4 Camera::ProjectionMatrix(VkExtent2D framebufferExtent,
                                   VkSurfaceTransformFlagBitsKHR preTransform) const {
    // ---- 1. aspect ratio, in the orientation the USER sees ----
    //
    // The swapchain extent is in the presentation engine's natural orientation,
    // which on a phone is portrait. Under a 90 or 270 degree rotation the image
    // the user is looking at is the transpose of that, so the camera has to
    // frame a scene of extent.height by extent.width. Get this backwards and
    // everything is stretched, in a way that looks like a bad FOV rather than
    // like a rotation bug.
    const bool axesSwapped = preTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                             preTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
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
    // The swapchain is created with preTransform = the surface's currentTransform
    // (see Swapchain.h), which is a promise to the presentation engine that the
    // image content arrives already rotated. That saves the compositor a full
    // screen blit on every single frame, which is the whole reason to do it — and
    // the price is that fulfilling the promise is now this function's job.
    //
    // It multiplies from the LEFT because it acts on clip space, after everything
    // else: rotate * proj * view * model. And it comes after the Y flip, so the
    // rotation is happening in Vulkan's y-down clip space, where a positive angle
    // reads as clockwise on screen — which is what ROTATE_90 means in the spec.
    //
    // NOTE: this is the one thing in this file that cannot be confirmed without a
    // device that actually rotates. If the scene comes out rotated the wrong way,
    // the fix is to negate `sine` in SurfaceRotation, and nothing else.
    return SurfaceRotation(preTransform) * projection;
}

} // namespace putorana::graphics
