// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/sky.hpp"
#include "f3d/scene/world.hpp"

#include <cmath>
#include <numbers>

using namespace f3d;

namespace {

constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;

/// The highest solar elevation of a day (sampled every minute) and its azimuth.
scene::HorizontalPosition solar_noon(scene::SkyTime time, double latitude, double longitude) {
    scene::HorizontalPosition best{-10.0f, 0.0f};
    for (int minute = 0; minute < 24 * 60; ++minute) {
        time.hours = minute / 60.0;
        const auto p = scene::solar_position(time, latitude, longitude);
        if (p.elevation > best.elevation) {
            best = p;
        }
    }
    return best;
}

} // namespace

TEST_CASE("sky: the noon sun is as high as the latitude and season say") {
    // Equinox on the equator: overhead.
    CHECK(solar_noon({2024, 3, 20, 0.0, 0.0}, 0.0, 0.0).elevation / kDeg > 88.5f);
    // June solstice at 45 N: 90 - 45 + 23.44.
    const auto june = solar_noon({2024, 6, 20, 0.0, 0.0}, 45.0, 0.0);
    CHECK(june.elevation / kDeg == doctest::Approx(68.44).epsilon(0.01));
    CHECK(june.azimuth / kDeg == doctest::Approx(180.0).epsilon(0.02)); // due south
    // December solstice at 45 N: 90 - 45 - 23.44.
    CHECK(solar_noon({2024, 12, 21, 0.0, 0.0}, 45.0, 0.0).elevation / kDeg == doctest::Approx(21.56).epsilon(0.02));
    // Southern hemisphere: the noon sun is in the north.
    const auto south = solar_noon({2024, 6, 20, 0.0, 0.0}, -33.9, 18.4);
    const float azimuth = south.azimuth / kDeg;
    CHECK((azimuth < 5.0f || azimuth > 355.0f));
}

TEST_CASE("sky: the sun rises in the east, sets in the west and is down at midnight") {
    const scene::SkyTime morning{2024, 6, 20, 7.0, 0.0};
    const auto m = scene::solar_position(morning, 45.0, 0.0);
    CHECK(m.elevation > 0.0f);
    CHECK(m.azimuth / kDeg > 45.0f);
    CHECK(m.azimuth / kDeg < 135.0f);

    const auto e = scene::solar_position({2024, 6, 20, 17.0, 0.0}, 45.0, 0.0);
    CHECK(e.elevation > 0.0f);
    CHECK(e.azimuth / kDeg > 225.0f);
    CHECK(e.azimuth / kDeg < 315.0f);

    CHECK(scene::solar_position({2024, 6, 20, 0.0, 0.0}, 45.0, 0.0).elevation < 0.0f);
    // A time zone shifts local time: 12:00 at UTC+2 on longitude 0 is 10:00 solar time.
    const auto shifted = scene::solar_position({2024, 6, 20, 12.0, 2.0}, 45.0, 0.0);
    CHECK(shifted.azimuth / kDeg < 160.0f);
}

TEST_CASE("sky: the moon's phase follows the calendar") {
    // Full moon 2024-03-25 07:00 UTC, new moon 2024-04-08 18:21 UTC, first quarter 2024-03-17 04:11 UTC.
    CHECK(scene::lunar_state({2024, 3, 25, 7.0, 0.0}, 45.0, 0.0).illumination > 0.95f);
    CHECK(scene::lunar_state({2024, 4, 8, 18.35, 0.0}, 45.0, 0.0).illumination < 0.05f);
    CHECK(scene::lunar_state({2024, 3, 17, 4.2, 0.0}, 45.0, 0.0).illumination == doctest::Approx(0.5).epsilon(0.2));
    // Near full moon the moon is up around midnight, opposite the sun.
    const auto midnight = scene::lunar_state({2024, 3, 25, 0.0, 0.0}, 45.0, 0.0);
    CHECK(midnight.position.elevation > 0.0f);
}

TEST_CASE("sky: directions use north -Z, east +X, up +Y") {
    const auto dir = [](float elevation_deg, float azimuth_deg, float north = 0.0f) {
        return scene::sky_direction({elevation_deg * kDeg, azimuth_deg * kDeg}, north);
    };
    const auto near = [](glm::vec3 a, glm::vec3 b) { return glm::length(a - b) < 1e-4f; };
    CHECK(near(dir(90.0f, 0.0f), {0.0f, 1.0f, 0.0f}));
    CHECK(near(dir(0.0f, 0.0f), {0.0f, 0.0f, -1.0f}));
    CHECK(near(dir(0.0f, 90.0f), {1.0f, 0.0f, 0.0f}));
    CHECK(near(dir(0.0f, 180.0f), {0.0f, 0.0f, 1.0f}));
    CHECK(near(dir(0.0f, 0.0f, 90.0f), {1.0f, 0.0f, 0.0f})); // north turned towards +X
    CHECK(near(dir(30.0f, 0.0f), {0.0f, 0.5f, -0.8660254f}));
}

TEST_CASE("sky: the active sky is the first visible one") {
    scene::World world;
    CHECK((scene::active_sky(world) == entt::null));
    const entt::entity a = world.create("Sky A");
    const entt::entity b = world.create("Sky B");
    world.registry().emplace<scene::Sky>(b);
    CHECK(scene::active_sky(world) == b);
    world.registry().emplace<scene::Sky>(a);
    CHECK(scene::active_sky(world) == a);

    const core::Guid hidden = core::Guid::generate();
    REQUIRE(world.layers().insert({hidden, "Hidden", false, false}));
    world.registry().emplace<scene::Layer>(a, scene::Layer{hidden});
    CHECK(scene::active_sky(world) == b);

    // The Sky's own celestial state at noon: the sun is up and to the south.
    const scene::Celestial c = scene::celestial(world.registry().get<scene::Sky>(b));
    CHECK(c.sun_elevation > 60.0f * kDeg);
    CHECK(c.sun_direction.z > 0.0f);
}
