// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/fog.hpp"
#include "f3d/render/raster/post_process.hpp"
#include "f3d/render/raster/render_settings.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace f3d;
using namespace f3d::render::raster;

TEST_CASE("fog: the analytic height-fog integral matches numerical integration") {
    scene::HeightFog fog;
    fog.density = 0.01f;
    fog.height_falloff = 0.05f;
    const float base = 10.0f;
    for (const glm::vec3& dir : {glm::normalize(glm::vec3(1.0f, 0.3f, 0.0f)), glm::normalize(glm::vec3(1.0f, -0.2f, 0.5f)),
                                 glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)}) {
        const glm::vec3 origin(0.0f, 25.0f, 0.0f);
        const float s0 = 5.0f, s1 = 120.0f;
        double numeric = 0.0;
        constexpr int kSteps = 20000;
        const double ds = (s1 - s0) / kSteps;
        for (int i = 0; i < kSteps; ++i) {
            const double s = s0 + (i + 0.5) * ds;
            const double y = origin.y + dir.y * s;
            numeric += fog.density * std::exp(-fog.height_falloff * (y - base)) * ds;
        }
        CAPTURE(dir.y);
        CHECK(height_fog_optical_depth(fog, base, origin, dir, s0, s1) ==
              doctest::Approx(numeric).epsilon(1e-3));
    }
    CHECK(height_fog_optical_depth(fog, base, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f, 5.0f) == 0.0f);
}

TEST_CASE("fog: froxel grid sizes by quality") {
    CHECK(fog_grid_size(1920, 1080, FogQuality::Off) == glm::uvec3(0u));
    CHECK(fog_grid_size(1920, 1080, FogQuality::High) == glm::uvec3(240, 135, 64));
    CHECK(fog_grid_size(1920, 1080, FogQuality::Low) == glm::uvec3(120, 68, 48));
    CHECK(fog_grid_size(1, 1, FogQuality::Epic) == glm::uvec3(1, 1, 96));
}

TEST_CASE("fog: the first visible HeightFog applies; fog volumes are gathered") {
    scene::register_types();
    scene::World world;
    const entt::entity hidden = world.create("Hidden fog");
    world.registry().emplace<scene::HeightFog>(hidden).visible = false;
    const entt::entity fog = world.create("Fog");
    world.registry().emplace<scene::HeightFog>(fog).density = 0.03f;
    world.set_local(fog, glm::vec3(0.0f, 12.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    const entt::entity volume = world.create("Mist");
    world.registry().emplace<scene::FogVolume>(volume).size = glm::vec3(10.0f, 4.0f, 10.0f);
    world.set_local(volume, glm::vec3(5.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

    const SceneFog gathered = gather_fog(world);
    REQUIRE(gathered.height);
    CHECK(gathered.height->density == doctest::Approx(0.03f));
    CHECK(gathered.base_height == doctest::Approx(12.0f));
    REQUIRE(gathered.volumes.size() == 1);
    CHECK(gathered.volumetric());
    // The box maps its corner (10, 2, 5) in world space to (0.5, 0.5, 0.5).
    const glm::vec4 corner = gathered.volumes[0].world_to_box * glm::vec4(10.0f, 2.0f, 5.0f, 1.0f);
    CHECK(corner.x == doctest::Approx(0.5f));
    CHECK(corner.y == doctest::Approx(0.5f));
    CHECK(corner.z == doctest::Approx(0.5f));
    CHECK(gathered.volumes[0].shape.y == doctest::Approx(4.0f));

    world.registry().get<scene::HeightFog>(fog).volumetric = false;
    world.registry().remove<scene::FogVolume>(volume);
    CHECK_FALSE(gather_fog(world).volumetric());
}

TEST_CASE("render settings: screen percentage and scalability") {
    CHECK(scaled_render_size(1920, 1080, 100.0f) == glm::uvec2(1920, 1080));
    CHECK(scaled_render_size(1920, 1080, 50.0f) == glm::uvec2(960, 540));
    CHECK(scaled_render_size(1920, 1080, 10.0f) == glm::uvec2(480, 270)); // clamped to 25 %
    CHECK(scaled_render_size(1, 1, 25.0f) == glm::uvec2(1, 1));
    RenderSettings settings;
    std::uint32_t shadows = 0;
    apply_scalability(Scalability::Low, settings, shadows);
    CHECK(settings.screen_percentage == 50.0f);
    CHECK(settings.fog_quality == FogQuality::Low);
    CHECK(shadows == 1024);
    apply_scalability(Scalability::Epic, settings, shadows);
    CHECK(settings.screen_percentage == 100.0f);
    CHECK(shadows == 4096);
    CHECK(parse_fog_quality("medium") == FogQuality::Medium);
    CHECK_FALSE(parse_fog_quality("ultra"));
    CHECK(parse_scalability(to_string(Scalability::High)) == Scalability::High);
}

TEST_CASE("post-processing: white balance and bloom levels") {
    const glm::mat3 identity = white_balance(6500.0f, 0.0f);
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            CHECK(identity[c][r] == doctest::Approx(c == r ? 1.0f : 0.0f).epsilon(1e-4));
        }
    }
    // Balanced for tungsten (3200 K), white comes out blue; for shade (9000 K), warm.
    const glm::vec3 cool = white_balance(3200.0f, 0.0f) * glm::vec3(1.0f);
    CHECK(cool.b > cool.r);
    const glm::vec3 warm = white_balance(9000.0f, 0.0f) * glm::vec3(1.0f);
    CHECK(warm.r > warm.b);
    // Positive tint: greener.
    const glm::vec3 green = white_balance(6500.0f, 1.0f) * glm::vec3(1.0f);
    CHECK(green.g > green.r);
    CHECK(green.g > green.b);

    CHECK(bloom_levels(0.0f, 1920, 1080) == 2);
    CHECK(bloom_levels(1.0f, 1920, 1080) == 7);
    CHECK(bloom_levels(1.0f, 16, 16) == 3); // 8, 4, 2
}
