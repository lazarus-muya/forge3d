// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/assets/mesh.hpp"
#include "f3d/assets/mesh_library.hpp"
#include "f3d/assets/raycast.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <vector>

using namespace f3d;

namespace {

assets::MeshData triangle_mesh() {
    assets::MeshData mesh;
    mesh.name = "tri";
    mesh.vertices = {
        {glm::vec3{-1.0f, -1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0xFFFFFFFFu},
        {glm::vec3{1.0f, -1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0xFFFFFFFFu},
        {glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0xFFFFFFFFu},
    };
    mesh.indices = {0, 1, 2};
    mesh.bounds.add(glm::vec3{-1.0f, -1.0f, 0.0f});
    mesh.bounds.add(glm::vec3{1.0f, 1.0f, 0.0f});
    return mesh;
}

} // namespace

TEST_CASE("raycast: ray_mesh intersects a triangle") {
    const assets::MeshData mesh = triangle_mesh();
    const assets::MeshHit hit = assets::ray_mesh(glm::vec3{0, 0, 5}, glm::vec3{0, 0, -1}, mesh);
    CHECK(hit.hit);
    CHECK(hit.t == doctest::Approx(5.0f));
    CHECK(hit.normal.z > 0.0f);

    CHECK_FALSE(assets::ray_mesh(glm::vec3{5, 5, 5}, glm::vec3{0, 0, -1}, mesh).hit);
}

TEST_CASE("raycast: raycast_scene finds the entity under the ray") {
    scene::World world;
    assets::MeshLibrary meshes;
    meshes.add("tri", triangle_mesh());

    const entt::entity e = world.create("tri");
    world.registry().emplace<scene::MeshRenderer>(e, scene::MeshRenderer{{core::Guid{}, "tri"}, true});
    world.set_local(e, glm::vec3{0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});

    const assets::SceneHit hit = assets::raycast_scene(world, meshes, glm::vec3{0, 0, 5}, glm::vec3{0, 0, -1});
    CHECK(hit.hit);
    CHECK(hit.entity == e);
    CHECK(std::abs(hit.point.z) < 0.001f);

    // A ray that misses every mesh.
    const assets::SceneHit miss = assets::raycast_scene(world, meshes, glm::vec3{0, 0, 5}, glm::vec3{1, 0, 0});
    CHECK_FALSE(miss.hit);
}

TEST_CASE("raycast: the scene BVH agrees with brute force and prunes by bounds") {
    scene::World world;
    assets::MeshLibrary meshes;
    meshes.add("tri", triangle_mesh());

    // A 10 x 10 grid of triangles facing +Z, at different depths.
    std::vector<entt::entity> entities;
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            const entt::entity e = world.create("tri");
            world.registry().emplace<scene::MeshRenderer>(e, scene::MeshRenderer{{core::Guid{}, "tri"}, true});
            world.set_local(e, glm::vec3{x * 3.0f, y * 3.0f, -static_cast<float>((x + y) % 4)},
                            glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});
            entities.push_back(e);
        }
    }
    assets::SceneBvh bvh;
    bvh.build(world, meshes);
    CHECK(bvh.size() == 100);
    CHECK(bvh.node_count() > 1);

    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            const glm::vec3 origin{x * 3.0f, y * 3.0f - 0.5f, 10.0f};
            const assets::SceneHit hit = bvh.raycast(origin, glm::vec3{0, 0, -1});
            REQUIRE(hit.hit);
            CHECK(hit.entity == entities[static_cast<std::size_t>(y * 10 + x)]);
            CHECK(hit.point.z == doctest::Approx(-static_cast<float>((x + y) % 4)));
            CHECK(bvh.last_meshes_tested() < 5); // only the meshes whose bounds the ray enters
        }
    }
    // Skipping the entity under the ray lets it pass through (nothing behind it here).
    const entt::entity first = entities[0];
    CHECK_FALSE(
        bvh.raycast(glm::vec3{0, -0.5f, 10}, glm::vec3{0, 0, -1}, [&](entt::entity e) { return e == first; }).hit);
    // Excluded at build time (a locked layer, for example).
    assets::SceneBvh partial;
    partial.build(world, meshes, [&](entt::entity e) { return e == first; });
    CHECK(partial.size() == 99);
    CHECK_FALSE(partial.raycast(glm::vec3{0, -0.5f, 10}, glm::vec3{0, 0, -1}).hit);
}

TEST_CASE("raycast: nearest_vertex snaps within the angle and ignores the rest") {
    scene::World world;
    assets::MeshLibrary meshes;
    meshes.add("tri", triangle_mesh());
    const entt::entity e = world.create("tri");
    world.registry().emplace<scene::MeshRenderer>(e, scene::MeshRenderer{{core::Guid{}, "tri"}, true});
    world.set_local(e, glm::vec3{10.0f, 0.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{2.0f});
    assets::SceneBvh bvh;
    bvh.build(world, meshes);

    // The top vertex (0, 1, 0) scaled by 2 and moved to x = 10: (10, 2, 0). Aim slightly off it.
    const glm::vec3 origin{10.0f, 2.0f, 10.0f};
    const assets::SceneHit hit = bvh.nearest_vertex(origin, glm::normalize(glm::vec3{0.01f, -0.01f, -1.0f}), 0.05f);
    REQUIRE(hit.hit);
    CHECK(hit.entity == e);
    CHECK(hit.point.x == doctest::Approx(10.0f));
    CHECK(hit.point.y == doctest::Approx(2.0f));
    CHECK(hit.point.z == doctest::Approx(0.0f));
    // Too far from any vertex for the angle.
    CHECK_FALSE(bvh.nearest_vertex(origin, glm::normalize(glm::vec3{0.3f, 0.0f, -1.0f}), 0.05f).hit);
}
