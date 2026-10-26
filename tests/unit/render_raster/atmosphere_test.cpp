// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/render/raster/atmosphere.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/sky.hpp"

#include <cmath>

using namespace f3d;
using namespace f3d::render::raster;

TEST_CASE("atmosphere: vertical transmittance matches the optical depth of each layer") {
    const AtmosphereParams a;
    // Straight up: Rayleigh and Mie are exponential (depth = coefficient x scale
    // height), ozone a tent of area half_width.
    const glm::vec3 depth = a.rayleigh_scattering * a.rayleigh_scale_height + a.mie_extinction * a.mie_scale_height +
                            a.ozone_absorption * a.ozone_half_width;
    const glm::vec3 t = transmittance(a, 0.0f, 1.0f);
    CHECK(t.r == doctest::Approx(std::exp(-depth.r)).epsilon(0.01));
    CHECK(t.g == doctest::Approx(std::exp(-depth.g)).epsilon(0.01));
    CHECK(t.b == doctest::Approx(std::exp(-depth.b)).epsilon(0.01));
    CHECK(t.b < t.g);
    CHECK(t.g < t.r); // blue is scattered most

    // Towards the horizon the path is far longer; below it the ground blocks the sun.
    const glm::vec3 low = transmittance(a, 0.0f, 0.05f);
    CHECK(low.b < t.b * 0.5f);
    CHECK(transmittance(a, 0.0f, -0.1f) == glm::vec3(0.0f));
    // Higher up there is less air above.
    CHECK(transmittance(a, 5.0f, 0.3f).b > transmittance(a, 0.0f, 0.3f).b);
}

TEST_CASE("atmosphere: the noon sky is blue and the sunset horizon red") {
    const AtmosphereParams a;
    const glm::vec3 noon_sun = glm::normalize(glm::vec3(0.3f, 0.9f, 0.2f));
    const glm::vec3 zenith = single_scattering(a, 0.001f, {0.0f, 1.0f, 0.0f}, noon_sun);
    CHECK(zenith.b > zenith.g);
    CHECK(zenith.g > zenith.r);

    const glm::vec3 low_sun = glm::normalize(glm::vec3(0.0f, 0.02f, -1.0f));
    const glm::vec3 towards_sun = single_scattering(a, 0.001f, glm::normalize(glm::vec3(0.0f, 0.03f, -1.0f)), low_sun);
    CHECK(towards_sun.r > towards_sun.b);

    // More haze (aerosols) makes the sky around the sun brighter and whiter.
    scene::Sky hazy;
    hazy.haze = 5.0f;
    const glm::vec3 near_sun = glm::normalize(noon_sun + glm::vec3(0.05f, 0.0f, 0.0f));
    const glm::vec3 clear = single_scattering(a, 0.001f, near_sun, noon_sun);
    const glm::vec3 haze = single_scattering(atmosphere_params(hazy), 0.001f, near_sun, noon_sun);
    CHECK(haze.r > clear.r);
    CHECK(haze.r / haze.b > clear.r / clear.b);
}

TEST_CASE("atmosphere: exposure follows the light (EV100)") {
    CHECK(exposure_from_ev100(0.0f) == doctest::Approx(1.0f / 1.2f));
    CHECK(exposure_from_ev100(1.0f) == doctest::Approx(0.5f / 1.2f));
    // "Sunny 16": about EV 15 in full daylight (100 000 lux).
    CHECK(ev100_from_illuminance(100000.0f) == doctest::Approx(15.29).epsilon(0.01));

    scene::Sky sky; // 21 June, 45 N
    const auto lighting_at = [&](float hours) {
        sky.time_of_day = hours;
        return sky_lighting(sky, scene::celestial(sky), atmosphere_params(sky), 2.0f);
    };
    const SkyLighting noon = lighting_at(12.0f);
    CHECK(noon.sun_illuminance.g > 90000.0f);
    CHECK(noon.sun_illuminance.g < 125000.0f);
    CHECK(noon.sky_illuminance.g > 2000.0f);
    CHECK(noon.ev100 > 14.5f);
    CHECK(noon.ev100 < 16.0f);
    CHECK_FALSE(noon.shadow_from_moon);

    const SkyLighting evening = lighting_at(18.0f);
    CHECK(evening.sun_illuminance.r > evening.sun_illuminance.b); // warmer
    CHECK(evening.ev100 < noon.ev100);

    const SkyLighting midnight = lighting_at(0.0f);
    CHECK(midnight.sun_illuminance == glm::vec3(0.0f));
    CHECK(midnight.ev100 == doctest::Approx(1.0f)); // clamped: night stays dark

    sky.exposure_compensation = 2.0f;
    CHECK(lighting_at(12.0f).ev100 == doctest::Approx(noon.ev100 - 2.0f));
}

TEST_CASE("atmosphere: sky constants carry the sky and the hemisphere fallback") {
    scene::Sky sky;
    const SkyLighting lighting = sky_lighting(sky, scene::celestial(sky), atmosphere_params(sky), 2.0f);
    const SkyConstants c = make_sky_constants(atmosphere_params(sky), sky, lighting, 2.0f);
    CHECK(c.ground_albedo.w == 1.0f);
    CHECK(c.planet.w == doctest::Approx(0.002f)); // camera height in km
    CHECK(glm::vec3(c.sun_direction) == lighting.sun_direction);
    CHECK(c.sun_direction.w == doctest::Approx(std::cos(kSunAngularRadius)));

    const SkyConstants h = make_hemisphere_constants({0.1f, 0.2f, 0.3f}, {0.4f, 0.5f, 0.6f});
    CHECK(h.ground_albedo.w == 0.0f);
    CHECK(glm::vec3(h.hemi_sky) == glm::vec3(0.1f, 0.2f, 0.3f));
    CHECK(glm::vec3(h.hemi_ground) == glm::vec3(0.4f, 0.5f, 0.6f));
}
