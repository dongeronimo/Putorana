#include "Renderable.h"

#include <limits>

namespace putorana::graphics {

Aabb Renderable::WorldBounds(const glm::mat4& worldMatrix) const {
    const Aabb& local = mesh_->bounds();

    Aabb world{glm::vec3(std::numeric_limits<float>::max()),
               glm::vec3(std::numeric_limits<float>::lowest())};
    for (int corner = 0; corner < 8; ++corner) {
        // The three bits of `corner` pick min or max on each axis, which walks
        // all eight in one loop.
        const glm::vec3 point((corner & 1) ? local.max.x : local.min.x,
                              (corner & 2) ? local.max.y : local.min.y,
                              (corner & 4) ? local.max.z : local.min.z);
        const glm::vec3 transformed = glm::vec3(worldMatrix * glm::vec4(point, 1.0f));
        world.min = glm::min(world.min, transformed);
        world.max = glm::max(world.max, transformed);
    }
    return world;
}

} // namespace putorana::graphics
