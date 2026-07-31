#include "Camera.h"

namespace putorana::graphics {

Camera::~Camera() = default;

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

bool SurfaceAxesSwapped(VkSurfaceTransformFlagBitsKHR transform) {
    return transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
           transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
}

} // namespace putorana::graphics
