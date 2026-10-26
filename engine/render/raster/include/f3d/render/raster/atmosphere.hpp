// SPDX-License-Identifier: Apache-2.0
#pragma once

// Physical atmosphere, CPU side (phases.md P4.3). The model is Hillaire's (EGSR
// 2020): Rayleigh and Mie scattering with exponential density, an ozone layer, a
// spherical planet. The GPU builds look-up tables from SkyConstants (shaders/
// atmosphere.hlsli); the functions here are the CPU reference of the same model
// (single scattering) and give the renderer what it needs on the CPU: the colour
// of sun- and moonlight at the ground, the light from the sky dome, and the
// exposure. Distances in km.

#include "f3d/core/math.hpp"

namespace f3d::scene {
struct Sky;
struct Celestial;
} // namespace f3d::scene

namespace f3d::render::raster {

/// Radiance stored in sky textures (sky-view LUT, environment and probe cubes) is
/// multiplied by this, so fp16 holds daylight (shaders: F3D_ENV_SCALE).
inline constexpr float kEnvScale = 1.0f / 4096.0f;

struct AtmosphereParams {
    float ground_radius = 6360.0f;
    float top_radius = 6460.0f;
    glm::vec3 rayleigh_scattering{5.802e-3f, 13.558e-3f, 33.1e-3f}; ///< 1/km at sea level
    float rayleigh_scale_height = 8.0f;
    glm::vec3 mie_scattering{3.996e-3f};
    glm::vec3 mie_extinction{4.440e-3f};
    float mie_scale_height = 1.2f;
    float mie_g = 0.8f;
    glm::vec3 ozone_absorption{0.650e-3f, 1.881e-3f, 0.085e-3f};
    float ozone_center = 25.0f;     ///< km
    float ozone_half_width = 15.0f; ///< km (tent profile)
    glm::vec3 ground_albedo{0.3f};
};

/// Earth's atmosphere with the Sky's haze and ground albedo.
[[nodiscard]] AtmosphereParams atmosphere_params(const scene::Sky& sky);

/// Transmittance from height `height_km` along a direction with cosine `cos_zenith`
/// to the top of the atmosphere; zero if the ray hits the ground.
[[nodiscard]] glm::vec3 transmittance(const AtmosphereParams& atmosphere, float height_km, float cos_zenith);

/// Single-scattered sky radiance seen from `height_km` along `view` (world, +Y up)
/// per unit of sun illuminance at the top of the atmosphere.
[[nodiscard]] glm::vec3 single_scattering(const AtmosphereParams& atmosphere, float height_km, const glm::vec3& view,
                                          const glm::vec3& sun);

/// Illuminance on an upward-facing surface from the sky dome (sun excluded), per
/// unit sun illuminance (single scattering).
[[nodiscard]] glm::vec3 sky_illuminance(const AtmosphereParams& atmosphere, float height_km, const glm::vec3& sun);

/// Exposure from EV100 (Lagarde & de Rousiers): 1 / (1.2 * 2^EV100).
[[nodiscard]] float exposure_from_ev100(float ev100) noexcept;
/// EV100 an incident-light meter reads for an illuminance (lux): log2(E * 100 / 250).
[[nodiscard]] float ev100_from_illuminance(float lux) noexcept;

/// What the renderer takes from a Sky each frame.
struct SkyLighting {
    glm::vec3 sun_direction{0.0f, 1.0f, 0.0f};
    glm::vec3 sun_illuminance{0.0f}; ///< lux at the camera (after the atmosphere)
    glm::vec3 moon_direction{0.0f, 1.0f, 0.0f};
    glm::vec3 moon_illuminance{0.0f}; ///< lux at the camera
    glm::vec3 moon_radiance{0.0f};    ///< radiance of the moon's disc (before the atmosphere)
    glm::vec3 sky_illuminance{0.0f};  ///< lux on the ground from the sky dome
    glm::vec3 ground_radiance{0.0f};  ///< radiance of the lit ground (albedo / pi x illuminance)
    glm::vec3 night_radiance{0.0f};   ///< airglow and starlight
    bool shadow_from_moon = false;    ///< the moon, not the sun, is the shadowing light
    float ev100 = 0.0f;               ///< automatic exposure (with compensation)
    float exposure = 1.0f;
};

inline constexpr float kSunAngularRadius = 0.004675f;  ///< radians
inline constexpr float kMoonAngularRadius = 0.004520f; ///< radians
inline constexpr float kFullMoonIlluminance = 0.27f;   ///< lux at the top of the atmosphere

/// Sun and moon light, sky and ground light and exposure for a Sky at a camera
/// height (metres above the ground).
[[nodiscard]] SkyLighting sky_lighting(const scene::Sky& sky, const scene::Celestial& celestial,
                                       const AtmosphereParams& atmosphere, float camera_height_m);

/// GPU layout of shaders/atmosphere.hlsli `SkyConstants`.
struct SkyConstants {
    glm::vec4 rayleigh_scattering; ///< rgb, w: scale height
    glm::vec4 mie_scattering;      ///< rgb, w: scale height
    glm::vec4 mie_extinction;      ///< rgb, w: g
    glm::vec4 ozone_absorption;    ///< rgb, w: centre
    glm::vec4 planet;              ///< ground radius, top radius, ozone half-width, camera height (km)
    glm::vec4 ground_albedo;       ///< rgb, w: 1 = physical sky, 0 = hemisphere colours
    glm::vec4 sun_direction;       ///< xyz, w: cos(angular radius)
    glm::vec4 sun_illuminance;     ///< rgb: lux at the top of the atmosphere
    glm::vec4 ground_radiance;
    glm::vec4 moon_direction; ///< xyz, w: cos(angular radius)
    glm::vec4 moon_radiance;
    glm::vec4 night_radiance;
    glm::vec4 hemi_sky;    ///< hemisphere mode: radiance from above
    glm::vec4 hemi_ground; ///< hemisphere mode: radiance from below
};
static_assert(sizeof(SkyConstants) == 14 * 16);

/// Constants for a physical sky.
[[nodiscard]] SkyConstants make_sky_constants(const AtmosphereParams& atmosphere, const scene::Sky& sky,
                                              const SkyLighting& lighting, float camera_height_m);
/// Constants for the hemisphere fallback (no Sky): sky and ground radiance.
[[nodiscard]] SkyConstants make_hemisphere_constants(const glm::vec3& sky_radiance, const glm::vec3& ground_radiance);

} // namespace f3d::render::raster
