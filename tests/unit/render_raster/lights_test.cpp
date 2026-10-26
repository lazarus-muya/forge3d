// SPDX-License-Identifier: Apache-2.0
// Light clustering (phases.md P4.2): lights land in the right clusters. CPU
// reference; integration/raster/cluster_test.cpp checks the GPU pass against it.
#include <doctest/doctest.h>

#include "f3d/render/raster/lights.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <random>

using namespace f3d;
using namespace f3d::render::raster;

namespace {

render::View perspective_view(std::uint32_t width = 1280, std::uint32_t height = 720) {
    render::View view;
    view.camera_position = glm::vec3(0.0f, 2.0f, 10.0f);
    view.view = glm::lookAtRH(view.camera_position, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    view.near_plane = 0.1f;
    view.far_plane = 500.0f;
    view.projection = glm::perspectiveRH_ZO(glm::radians(60.0f), static_cast<float>(width) / static_cast<float>(height),
                                            view.near_plane, view.far_plane);
    view.width = width;
    view.height = height;
    return view;
}

render::View ortho_view() {
    render::View view = perspective_view(800, 600);
    view.orthographic = true;
    view.projection = glm::orthoRH_ZO(-8.0f, 8.0f, -6.0f, 6.0f, view.near_plane, view.far_plane);
    return view;
}

/// The cluster of a world-space point, through the projection (as the pixel shader finds it).
std::optional<std::uint32_t> cluster_of(const render::View& view, const glm::vec3& world) {
    const glm::vec4 clip = view.view_projection() * glm::vec4(world, 1.0f);
    if (clip.w <= 0.0f) {
        return std::nullopt;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f) {
        return std::nullopt; // off screen
    }
    const float px = (ndc.x * 0.5f + 0.5f) * static_cast<float>(view.width);
    const float py = (0.5f - ndc.y * 0.5f) * static_cast<float>(view.height);
    const float depth = -(view.view * glm::vec4(world, 1.0f)).z;
    return cluster_at(view, px, py, depth);
}

void check_conservative(const render::View& view) {
    // Random lights; every point inside a light's sphere must find the light in its cluster.
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> pos(-12.0f, 12.0f);
    std::uniform_real_distribution<float> range(0.3f, 4.0f);
    std::vector<GpuLight> lights;
    for (int i = 0; i < 40; ++i) {
        GpuLight l;
        l.position = glm::vec3(pos(rng), pos(rng) * 0.5f + 2.0f, pos(rng) - 10.0f);
        l.range = range(rng);
        lights.push_back(l);
    }
    const auto clusters = assign_lights(view, lights);
    REQUIRE(clusters.size() == kClusterCount);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    int checked = 0;
    for (std::uint32_t i = 0; i < lights.size(); ++i) {
        for (int s = 0; s < 400; ++s) {
            glm::vec3 offset(unit(rng), unit(rng), unit(rng));
            if (glm::length(offset) > 1.0f) {
                continue;
            }
            const auto cluster = cluster_of(view, lights[i].position + offset * lights[i].range * 0.999f);
            if (!cluster) {
                continue;
            }
            const auto& list = clusters[*cluster];
            INFO("light ", i, " cluster ", *cluster);
            CHECK(std::find(list.begin(), list.end(), i) != list.end());
            ++checked;
        }
    }
    CHECK(checked > 1000);
}

} // namespace

TEST_CASE("clusters: depth slices are exponential and cover near..far") {
    const render::View view = perspective_view();
    const ClusterDepth depth = cluster_depth(view);
    CHECK(cluster_slice(view.near_plane, depth) == 0);
    CHECK(cluster_slice(view.far_plane * 0.999f, depth) == kClusterSlices - 1);
    CHECK(cluster_slice(view.far_plane * 10.0f, depth) == kClusterSlices - 1); // clamped
    CHECK(cluster_slice(0.0f, depth) == 0);
    // Equal depth ratios span equal slice counts.
    const float ratio = std::pow(view.far_plane / view.near_plane, 1.0f / kClusterSlices);
    for (std::uint32_t k = 0; k < kClusterSlices; ++k) {
        const float mid = view.near_plane * std::pow(ratio, static_cast<float>(k) + 0.5f);
        CHECK(cluster_slice(mid, depth) == k);
    }
}

TEST_CASE("clusters: a cluster's box contains the points that map to it") {
    const render::View view = perspective_view();
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> px(0.0f, 1279.0f), py(0.0f, 719.0f), d(0.2f, 400.0f);
    for (int i = 0; i < 2000; ++i) {
        const float x = px(rng), y = py(rng), depth = d(rng);
        const std::uint32_t c = cluster_at(view, x, y, depth);
        glm::vec3 origin, direction;
        view.pixel_ray(x, y, origin, direction);
        // The view-space point at that depth along the pixel's ray.
        const glm::vec3 o = glm::vec3(view.view * glm::vec4(origin, 1.0f));
        const glm::vec3 dir = glm::mat3(view.view) * direction;
        const glm::vec3 p = o + dir * ((-depth - o.z) / dir.z);
        const Aabb box = cluster_bounds(view, c % kClusterTilesX, (c / kClusterTilesX) % kClusterTilesY,
                                        c / (kClusterTilesX * kClusterTilesY));
        INFO("pixel (", x, ", ", y, ") depth ", depth);
        CHECK(sphere_intersects(box, p, 1e-3f * depth));
    }
}

TEST_CASE("clusters: lights land in every cluster they touch (perspective)") {
    check_conservative(perspective_view());
}

TEST_CASE("clusters: lights land in every cluster they touch (orthographic)") {
    check_conservative(ortho_view());
}

TEST_CASE("clusters: a small light lands only near itself") {
    const render::View view = perspective_view();
    GpuLight light;
    light.position = glm::vec3(0.0f, 1.0f, 0.0f); // the view target, 10 m away
    light.range = 0.5f;
    const auto clusters = assign_lights(view, std::vector<GpuLight>{light});
    std::size_t hits = 0;
    for (std::uint32_t c = 0; c < kClusterCount; ++c) {
        if (!clusters[c].empty()) {
            ++hits;
            // Its clusters are around the screen centre and around 10 m deep.
            const std::uint32_t x = c % kClusterTilesX;
            const std::uint32_t y = (c / kClusterTilesX) % kClusterTilesY;
            const std::uint32_t z = c / (kClusterTilesX * kClusterTilesY);
            CHECK(x >= 6);
            CHECK(x <= 9);
            CHECK(y >= 3);
            CHECK(y <= 5);
            // The light spans depths 9.55..10.55 (it is 10.05 m away).
            CHECK(z >= cluster_slice(9.5f, cluster_depth(view)));
            CHECK(z <= cluster_slice(10.6f, cluster_depth(view)));
        }
    }
    CHECK(hits >= 1);
    CHECK(hits <= 8);
    // A light behind the camera touches nothing.
    light.position = glm::vec3(0.0f, 2.0f, 20.0f);
    const auto behind = assign_lights(view, std::vector<GpuLight>{light});
    CHECK(std::all_of(behind.begin(), behind.end(), [](const auto& list) { return list.empty(); }));
}

TEST_CASE("clusters: a cluster holds at most kMaxLightsPerCluster lights") {
    const render::View view = perspective_view();
    std::vector<GpuLight> lights(kMaxLightsPerCluster + 20);
    for (GpuLight& l : lights) {
        l.position = glm::vec3(0.0f, 1.0f, 0.0f);
        l.range = 2.0f;
    }
    const auto clusters = assign_lights(view, lights);
    const auto busiest = std::max_element(clusters.begin(), clusters.end(),
                                          [](const auto& a, const auto& b) { return a.size() < b.size(); });
    CHECK(busiest->size() == kMaxLightsPerCluster);
    CHECK(busiest->front() == 0); // in light order
}

TEST_CASE("lights: gathered from the scene with their transforms") {
    scene::World world;
    const entt::entity sun = world.create("Sun");
    world.registry().emplace<scene::Light>(
        sun, scene::Light{scene::LightType::Directional, glm::vec3(1.0f, 0.9f, 0.8f), 2.0f});
    // Rotated so local -Z points down.
    world.set_local(sun, glm::vec3(0.0f), glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
                    glm::vec3(1.0f));
    const entt::entity lamp = world.create("Lamp");
    world.registry().emplace<scene::Light>(lamp, scene::Light{scene::LightType::Point, glm::vec3(1.0f), 5.0f, 3.0f});
    world.set_local(lamp, glm::vec3(1.0f, 2.0f, 3.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    const entt::entity spot = world.create("Spot", lamp); // child: inherits the lamp's position
    scene::Light spot_light{scene::LightType::Spot, glm::vec3(0.5f), 4.0f, 6.0f, 0.2f, 0.5f};
    world.registry().emplace<scene::Light>(spot, spot_light);
    const entt::entity hidden = world.create("Hidden");
    world.registry().emplace<scene::Light>(hidden, scene::Light{});
    const core::Guid layer = core::Guid::generate();
    REQUIRE(world.layers().insert({layer, "Off", false, false}));
    world.registry().emplace<scene::Layer>(hidden, scene::Layer{layer});

    const SceneLights lights = gather_lights(world);
    REQUIRE(lights.directional.size() == 1);
    CHECK(lights.directional[0].direction.y == doctest::Approx(1.0f)); // towards the light: up
    CHECK(lights.directional[0].color.r == doctest::Approx(2.0f));
    REQUIRE(lights.local.size() == 2); // the hidden one is skipped
    CHECK(lights.local[0].position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(lights.local[0].range == 3.0f);
    CHECK(lights.local[0].color == glm::vec3(5.0f));
    CHECK(lights.local[0].type == 0);
    CHECK(lights.local[1].position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(lights.local[1].type == 1);
    CHECK(lights.local[1].direction.z == doctest::Approx(-1.0f));
    // Full intensity inside the inner cone, none outside the outer one.
    const auto falloff = [&](float angle) {
        return std::clamp(std::cos(angle) * lights.local[1].spot_scale + lights.local[1].spot_offset, 0.0f, 1.0f);
    };
    CHECK(falloff(0.1f) == doctest::Approx(1.0f));
    CHECK(falloff(0.6f) == doctest::Approx(0.0f));
    CHECK(falloff(0.35f) > 0.0f);
    CHECK(falloff(0.35f) < 1.0f);
}
