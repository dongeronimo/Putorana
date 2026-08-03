#ifndef PUTORANA_RECON_UNPROJECT_H
#define PUTORANA_RECON_UNPROJECT_H

#include "ar/Subsystem.h"

#include <glm/glm.hpp>

#include <optional>

/**
 * Turning what the camera measures into a surface. See README.md — the
 * mathematics is explained there, and this header assumes it.
 *
 * ## Nothing in here includes volk.h or open_chisel
 *
 * The first rule is inherited from putorana::ar: this is plain numbers, and
 * turning them into anything Vulkan understands happens elsewhere.
 *
 * The second is local and is about Eigen. When OpenChisel arrives behind this
 * namespace, the .cpp files that include it must be compiled with NDEBUG and
 * -O3 (see third_party/open_chisel/CMakeLists.txt), and NO Eigen type may cross
 * back out through a header — mixing NDEBUG across translation units that share
 * an Eigen header is an ODR violation. This file is GLM and plain structs, which
 * is what that boundary looks like from the outside.
 * */
namespace putorana::recon {

/**
 * One depth sample turned into a point in ARCore's world space.
 *
 * `u` and `v` are pixel coordinates in the DEPTH image, not the camera image and
 * not the display. `pose` must be the SENSOR pose (`CameraFrame::sensorPose`),
 * because that is the one whose axes agree with the unrotated intrinsics carried
 * in `depth.intrinsics`. Passing displayPose here compiles, runs, and produces a
 * reconstruction rotated by a multiple of 90 degrees — see README.md, "Which
 * pose — the trap this section exists for".
 *
 * Returns nothing when the sample has no estimate. That is not an error: ARCore
 * leaves 0 wherever it could not measure, and on a typical indoor frame a real
 * fraction of the image is 0. A caller that treats it as an error will spend a
 * while wondering why the reconstruction is empty; a caller that treats 0 as a
 * distance will put a wall through the camera.
 * */
std::optional<glm::vec3> Unproject(const ar::DepthImage& depth,
                                   const ar::CameraPose& pose,
                                   int32_t u,
                                   int32_t v);

/**
 * The middle of the depth map, which is the cheapest end-to-end test there is:
 * point the phone at a wall a known distance away and the answer should land on
 * the wall.
 *
 * It is worth being clear about how much this proves. It exercises the units,
 * both axis flips, the quaternion order and the choice of pose simultaneously,
 * with geometry simple enough that a wrong answer is obvious rather than
 * plausible. It does NOT test fy independently of fx, or the row stride, because
 * the centre pixel is where every one of those errors is zero. That is what the
 * full point cloud is for.
 * */
std::optional<glm::vec3> UnprojectCentre(const ar::DepthImage& depth,
                                         const ar::CameraPose& pose);

} // namespace putorana::recon

#endif // PUTORANA_RECON_UNPROJECT_H
