// SPDX-License-Identifier: Apache-2.0
#pragma once

// Snapping and ray/triangle intersection (phases.md P2.4). Pure math, no scene
// or asset dependencies, so it is unit-testable.

#include "f3d/core/math.hpp"

namespace f3d::core {

/// Snaps a scalar to the nearest multiple of `step` (step > 0).
[[nodiscard]] float snap_grid(float value, float step);

/// Snaps each component to the nearest multiple of the matching step.
[[nodiscard]] glm::vec3 snap_grid(const glm::vec3& value, const glm::vec3& step);

/// Slab test of the ray `origin + t * direction` against `box`. On a hit, sets
/// `t_near` to the entry distance (0 if the origin is inside) and returns true.
/// Only hits with t in [0, t_max] count.
[[nodiscard]] bool ray_aabb(const glm::vec3& origin, const glm::vec3& direction, const Bounds& box, float t_max,
                            float& t_near);

/// Distance from `point` to the ray (`direction` normalised). Sets `t` to the
/// distance along the ray of the closest point (clamped to >= 0).
[[nodiscard]] float ray_point_distance(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& point,
                                       float& t);

/// Möller–Trumbore ray/triangle intersection. Returns true and sets `t` (>= 0)
/// and the barycentric coordinates `u`, `v` (with `w = 1 - u - v`) on a hit.
[[nodiscard]] bool ray_triangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& v0,
                                const glm::vec3& v1, const glm::vec3& v2, float& t, float& u, float& v);

} // namespace f3d::core
