#include "Unproject.h"

#include <glm/gtc/quaternion.hpp>

namespace putorana::recon {

std::optional<glm::vec3> Unproject(const ar::DepthImage& depth,
                                   const ar::CameraPose& pose,
                                   int32_t u,
                                   int32_t v) {
    if (depth.millimetres == nullptr || u < 0 || v < 0 || u >= depth.width || v >= depth.height) {
        return std::nullopt;
    }

    const ar::Intrinsics& k = depth.intrinsics;
    if (k.fx <= 0.0f || k.fy <= 0.0f) {
        return std::nullopt;
    }

    // --- reading the sample ---
    //
    // Through the BYTE stride, not the width. They are equal on the device this
    // was written against (160 wide, 320 bytes) and there is no promise they
    // stay that way; a padded row read as if it were tight produces an image
    // that shears progressively down the frame, which looks like a pose bug.
    const auto* row = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(depth.millimetres) + v * depth.rowStrideBytes);
    const uint16_t millimetres = row[u];
    if (millimetres == 0) {
        // No estimate here. Not an error — see the header.
        return std::nullopt;
    }
    const float d = static_cast<float>(millimetres) * 0.001f;

    // --- Step 1: pixel to camera space, in the CV convention ---
    //
    // The pinhole model inverted. d is the distance along the principal axis,
    // NOT the distance from the camera to the point: a sample at the edge of the
    // frame is further away than its depth value says, and this is exactly the
    // step where that gets accounted for, because x and y grow with the offset
    // from the principal point while z does not.
    //
    // +0.5f puts the sample at the CENTRE of its pixel rather than its corner.
    // Half a pixel is nothing on one point and is a consistent bias across a
    // whole cloud, which is the kind of error that is impossible to see and
    // annoying to find later.
    const float xCv = (static_cast<float>(u) + 0.5f - k.cx) / k.fx * d;
    const float yCv = (static_cast<float>(v) + 0.5f - k.cy) / k.fy * d;
    const float zCv = d;

    // --- Step 2: into ARCore's axis convention ---
    //
    // The intrinsics are computer-vision convention: +X right, +Y DOWN, +Z
    // FORWARD out of the lens. ARCore's camera space is OpenGL convention: +X
    // right, +Y UP, +Z BACKWARD, so the camera looks down its own -Z.
    //
    // Two sign flips, and they are not optional. Flipping neither puts the
    // reconstruction behind the camera and upside down; flipping only Y puts it
    // behind the camera. Both failures look like a broken pose rather than a
    // convention error, which is why they are spelled out here instead of being
    // folded into the expressions above.
    const glm::vec3 inCamera(xCv, -yCv, -zCv);

    // --- Step 3: camera space to world space ---
    //
    // The pose is camera-to-world already, so this is a rotation followed by a
    // translation and not an inverse of anything.
    //
    // glm::quat's constructor takes w FIRST; ARCore's raw array has it LAST.
    // Getting this wrong yields a rotation that is plausible from any single
    // still frame and wrong the moment the phone moves — the same trap
    // ArCamera.cpp documents on the rendering side.
    const glm::quat rotation(pose.rotation[3], pose.rotation[0], pose.rotation[1],
                             pose.rotation[2]);
    const glm::vec3 translation(pose.translation[0], pose.translation[1], pose.translation[2]);

    return rotation * inCamera + translation;
}

std::optional<glm::vec3> UnprojectCentre(const ar::DepthImage& depth,
                                         const ar::CameraPose& pose) {
    return Unproject(depth, pose, depth.width / 2, depth.height / 2);
}

} // namespace putorana::recon
