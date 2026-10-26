// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/snap.hpp"

#include <cmath>

using f3d::core::ray_triangle;
using f3d::core::snap_grid;

TEST_CASE("snap: grid rounds to the nearest multiple") {
    CHECK(snap_grid(0.49f, 1.0f) == doctest::Approx(0.0f));
    CHECK(snap_grid(0.51f, 1.0f) == doctest::Approx(1.0f));
    CHECK(snap_grid(-0.49f, 1.0f) == doctest::Approx(0.0f));
    CHECK(snap_grid(-0.51f, 1.0f) == doctest::Approx(-1.0f));
    CHECK(snap_grid(1.5f, 0.5f) == doctest::Approx(1.5f));
    CHECK(snap_grid(1.24f, 0.25f) == doctest::Approx(1.25f));
    CHECK(snap_grid(3.0f, 0.0f) == doctest::Approx(3.0f)); // step <= 0: no snapping

    const glm::vec3 v = snap_grid(glm::vec3{1.24f, 2.76f, -3.31f}, glm::vec3{0.5f, 1.0f, 0.25f});
    CHECK(v.x == doctest::Approx(1.0f));
    CHECK(v.y == doctest::Approx(3.0f));
    CHECK(v.z == doctest::Approx(-3.25f));
}

TEST_CASE("snap: ray_triangle Möller–Trumbore") {
    const glm::vec3 v0{-1.0f, -1.0f, 0.0f};
    const glm::vec3 v1{1.0f, -1.0f, 0.0f};
    const glm::vec3 v2{0.0f, 1.0f, 0.0f};
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;

    CHECK(ray_triangle(glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, v0, v1, v2, t, u, v));
    CHECK(t == doctest::Approx(5.0f));
    CHECK(u + v <= 1.0f);

    CHECK(ray_triangle(glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, v0, v1, v2, t, u, v));
    CHECK(t == doctest::Approx(5.0f));
    CHECK(u + v > 0.0f);

    CHECK(ray_triangle(glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, v0, v1, v2, t, u, v));
    CHECK(t == doctest::Approx(5.0f));

    // Misses: outside the triangle, ray behind origin, and parallel to the plane.
    CHECK_FALSE(ray_triangle(glm::vec3{5.0f, 5.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, v0, v1, v2, t, u, v));
    CHECK_FALSE(ray_triangle(glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, v0, v1, v2, t, u, v));
    CHECK_FALSE(ray_triangle(glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{1.0f, 0.0f, 0.0f}, v0, v1, v2, t, u, v));
}

TEST_CASE("snap: ray_aabb slab test") {
    f3d::core::Bounds box;
    box.add(glm::vec3{-1.0f});
    box.add(glm::vec3{1.0f});
    float t = 0.0f;
    using f3d::core::ray_aabb;

    CHECK(ray_aabb(glm::vec3{0, 0, 5}, glm::vec3{0, 0, -1}, box, 100.0f, t));
    CHECK(t == doctest::Approx(4.0f));
    // Origin inside: entry distance 0.
    CHECK(ray_aabb(glm::vec3{0.5f, 0, 0}, glm::vec3{1, 0, 0}, box, 100.0f, t));
    CHECK(t == doctest::Approx(0.0f));
    // Axis-parallel ray outside the slab, a ray pointing away, and one stopped by t_max.
    CHECK_FALSE(ray_aabb(glm::vec3{2, 0, 5}, glm::vec3{0, 0, -1}, box, 100.0f, t));
    CHECK_FALSE(ray_aabb(glm::vec3{0, 0, 5}, glm::vec3{0, 0, 1}, box, 100.0f, t));
    CHECK_FALSE(ray_aabb(glm::vec3{0, 0, 5}, glm::vec3{0, 0, -1}, box, 3.0f, t));
    // Diagonal hit through a corner region.
    CHECK(ray_aabb(glm::vec3{-3, -3, 0}, glm::normalize(glm::vec3{1, 1, 0}), box, 100.0f, t));
    CHECK(t == doctest::Approx(std::sqrt(8.0f)));
    // Empty boxes never hit.
    CHECK_FALSE(ray_aabb(glm::vec3{0}, glm::vec3{1, 0, 0}, f3d::core::Bounds{}, 100.0f, t));
}

TEST_CASE("snap: ray_point_distance") {
    float t = 0.0f;
    const float d = f3d::core::ray_point_distance(glm::vec3{0}, glm::vec3{0, 0, -1}, glm::vec3{3, 0, -4}, t);
    CHECK(d == doctest::Approx(3.0f));
    CHECK(t == doctest::Approx(4.0f));
    // Behind the origin: the closest point is the origin itself.
    CHECK(f3d::core::ray_point_distance(glm::vec3{0}, glm::vec3{0, 0, -1}, glm::vec3{0, 0, 2}, t) ==
          doctest::Approx(2.0f));
    CHECK(t == doctest::Approx(0.0f));
}
