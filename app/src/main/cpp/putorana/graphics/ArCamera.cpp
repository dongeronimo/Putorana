#include "ArCamera.h"

#include "Node.h"

#include <glm/gtc/type_ptr.hpp>

namespace putorana::graphics {

std::unique_ptr<ArCamera> ArCamera::Create() {
    return std::unique_ptr<ArCamera>(new ArCamera());
}

void ArCamera::SetFrame(const ar::CameraFrame& frame, Node& node) {
    // ---- the projection ----
    //
    // Column-major floats straight into a mat4: the same memory order glm uses,
    // so this is a copy and not a transpose.
    glm::mat4 projection = glm::make_mat4(frame.projection);

    // Clip Z from OpenGL's [-1,1] to Vulkan's [0,1]: z' = (z + w) / 2. Written
    // as a row operation over every column rather than as a matrix multiply,
    // because that is literally all it is.
    //
    // Skipping this does not black the screen — it halves the usable depth range
    // and quietly clips everything in front of the midpoint, which reads as
    // geometry disappearing when it gets close.
    for (int column = 0; column < 4; ++column) {
        projection[column][2] = 0.5f * projection[column][2] + 0.5f * projection[column][3];
    }

    // Vulkan's Y points DOWN where OpenGL's points up, so row 1 is negated. The
    // WHOLE row, not just [1][1]: ARCore's projection carries a principal point
    // offset, so unlike a textbook perspective matrix it has terms outside the
    // diagonal, and negating only the diagonal one would leave the image
    // off-centre by however far the sensor's optical axis is from the middle.
    for (int column = 0; column < 4; ++column) {
        projection[column][1] = -projection[column][1];
    }

    projection_ = projection;

    // ---- the pose ----
    tracking_ = frame.tracking;
    if (!tracking_) {
        return;
    }

    // displayPose, not sensorPose. This is the RENDERING half of the app, and
    // the display-oriented pose is the one that agrees with the projection
    // matrix above. Reconstruction takes the other one — see Subsystem.h.
    const ar::CameraPose& pose = frame.displayPose;
    node.position = glm::vec3(pose.translation[0], pose.translation[1], pose.translation[2]);
    // ARCore's raw order is qx, qy, qz, qw; glm::quat's constructor takes w
    // first. Getting this wrong produces a rotation that looks plausible and is
    // wrong in a way no still frame reveals.
    node.SetRotation(glm::quat(pose.rotation[3], pose.rotation[0], pose.rotation[1],
                               pose.rotation[2]));
}

glm::mat4 ArCamera::ProjectionMatrix(VkExtent2D /*framebufferExtent*/,
                                     VkSurfaceTransformFlagBitsKHR preTransform) const {
    // No aspect ratio arithmetic anywhere here, and that is the point. ARCore was
    // told the viewport's shape through ArSession_setDisplayGeometry and built
    // this matrix for it, so the extent is already accounted for — recomputing an
    // aspect from it would be a second opinion, and two opinions about the same
    // frustum is exactly how the virtual content stops matching the picture.
    //
    // The pre-rotation still has to be applied. ARCore answers in the VIEW's
    // space — what the user sees — while this is drawn into a swapchain that
    // carries a preTransform, and those differ by exactly this rotation.
    // CameraFeed applies the same one to its texture coordinates; if these two
    // ever disagree, the cube and the camera image rotate away from each other.
    return SurfaceRotation(preTransform) * projection_;
}

} // namespace putorana::graphics
