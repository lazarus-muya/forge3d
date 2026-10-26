// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/snap.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace f3d::core {

float snap_grid(float value, float step) {
    if (step <= 0.0f) {
        return value;
    }
    return std::round(value / step) * step;
}

glm::vec3 snap_grid(const glm::vec3& value, const glm::vec3& step) {
    return {snap_grid(value.x, step.x), snap_grid(value.y, step.y), snap_grid(value.z, step.z)};
}

bool ray_aabb(const glm::vec3& origin, const glm::vec3& direction, const Bounds& box, float t_max, float& t_near) {
    if (box.empty()) {
        return false;
    }
    float t0 = 0.0f;
    float t1 = t_max;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-12f) {
            // Parallel to the slab: inside it or never.
            if (origin[axis] < box.min[axis] || origin[axis] > box.max[axis]) {
                return false;
            }
            continue;
        }
        const float inv = 1.0f / direction[axis];
        float near_t = (box.min[axis] - origin[axis]) * inv;
        float far_t = (box.max[axis] - origin[axis]) * inv;
        if (near_t > far_t) {
            std::swap(near_t, far_t);
        }
        t0 = std::max(t0, near_t);
        t1 = std::min(t1, far_t);
        if (t0 > t1) {
            return false;
        }
    }
    t_near = t0;
    return true;
}

float ray_point_distance(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& point, float& t) {
    t = std::max(0.0f, glm::dot(point - origin, direction));
    return glm::distance(origin + direction * t, point);
}

bool ray_triangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& v0, const glm::vec3& v1,
                  const glm::vec3& v2, float& t, float& u, float& v) {
    constexpr float kEpsilon = 1e-7f;
    const glm::vec3 edge1 = v1 - v0;
    const glm::vec3 edge2 = v2 - v0;
    const glm::vec3 pvec = glm::cross(direction, edge2);
    const float det = glm::dot(edge1, pvec);
    if (std::abs(det) < kEpsilon) {
        return false;
    }
    const float inv_det = 1.0f / det;
    const glm::vec3 tvec = origin - v0;
    u = glm::dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 qvec = glm::cross(tvec, edge1);
    v = glm::dot(direction, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    t = glm::dot(edge2, qvec) * inv_det;
    return t >= 0.0f;
}

} // namespace f3d::core
