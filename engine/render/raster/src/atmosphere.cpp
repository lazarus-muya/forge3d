// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/atmosphere.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/sky.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace f3d::render::raster {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

struct Medium {
    glm::vec3 rayleigh{0.0f};
    glm::vec3 mie{0.0f};
    glm::vec3 extinction{0.0f};
};

Medium medium_at(const AtmosphereParams& a, float height) {
    const float rayleigh = std::exp(-height / a.rayleigh_scale_height);
    const float mie = std::exp(-height / a.mie_scale_height);
    const float ozone = std::max(0.0f, 1.0f - std::abs(height - a.ozone_center) / a.ozone_half_width);
    Medium m;
    m.rayleigh = a.rayleigh_scattering * rayleigh;
    m.mie = a.mie_scattering * mie;
    m.extinction = m.rayleigh + a.mie_extinction * mie + a.ozone_absorption * ozone;
    return m;
}

/// Distance to a sphere around the planet's centre along a ray, or -1.
float ray_sphere(const glm::vec3& origin, const glm::vec3& dir, float radius) {
    const float b = glm::dot(origin, dir);
    const float c = glm::dot(origin, origin) - radius * radius;
    const float d = b * b - c;
    if (d < 0.0f) {
        return -1.0f;
    }
    const float s = std::sqrt(d);
    if (-b - s >= 0.0f) {
        return -b - s;
    }
    return -b + s >= 0.0f ? -b + s : -1.0f;
}

float rayleigh_phase(float c) {
    return 3.0f / (16.0f * kPi) * (1.0f + c * c);
}

float mie_phase(float c, float g) {
    const float g2 = g * g;
    const float k = 3.0f / (8.0f * kPi) * (1.0f - g2) / (2.0f + g2);
    return k * (1.0f + c * c) / std::pow(std::max(1.0f + g2 - 2.0f * g * c, 1e-4f), 1.5f);
}

glm::vec3 transmittance_at(const AtmosphereParams& a, const glm::vec3& p, const glm::vec3& dir) {
    const float r = glm::length(p);
    return transmittance(a, r - a.ground_radius, glm::dot(p / r, dir));
}

glm::vec3 exp3(const glm::vec3& v) {
    return {std::exp(v.x), std::exp(v.y), std::exp(v.z)};
}

} // namespace

AtmosphereParams atmosphere_params(const scene::Sky& sky) {
    AtmosphereParams a;
    const float haze = std::max(sky.haze, 0.0f);
    a.mie_scattering *= haze;
    a.mie_extinction *= haze;
    a.ground_albedo = glm::clamp(sky.ground_albedo, glm::vec3(0.0f), glm::vec3(1.0f));
    return a;
}

glm::vec3 transmittance(const AtmosphereParams& a, float height_km, float cos_zenith) {
    const float r = a.ground_radius + std::max(height_km, 0.0f);
    const glm::vec3 origin(0.0f, r, 0.0f);
    const float mu = std::clamp(cos_zenith, -1.0f, 1.0f);
    const glm::vec3 dir(std::sqrt(std::max(1.0f - mu * mu, 0.0f)), mu, 0.0f);
    if (ray_sphere(origin, dir, a.ground_radius) > 0.0f) {
        return glm::vec3(0.0f);
    }
    const float length = std::max(ray_sphere(origin, dir, a.top_radius), 0.0f);
    constexpr int kSteps = 32;
    const float dt = length / kSteps;
    glm::vec3 depth(0.0f);
    for (int i = 0; i < kSteps; ++i) {
        const glm::vec3 p = origin + dir * ((static_cast<float>(i) + 0.5f) * dt);
        depth += medium_at(a, glm::length(p) - a.ground_radius).extinction * dt;
    }
    return exp3(-depth);
}

glm::vec3 single_scattering(const AtmosphereParams& a, float height_km, const glm::vec3& view, const glm::vec3& sun) {
    const glm::vec3 p(0.0f, a.ground_radius + std::max(height_km, 0.001f), 0.0f);
    const glm::vec3 dir = glm::normalize(view);
    const float t_ground = ray_sphere(p, dir, a.ground_radius);
    const float t = t_ground > 0.0f ? t_ground : std::max(ray_sphere(p, dir, a.top_radius), 0.0f);
    const float c = glm::dot(dir, sun);
    const float phase_r = rayleigh_phase(c);
    const float phase_m = mie_phase(c, a.mie_g);

    constexpr int kSteps = 24;
    glm::vec3 throughput(1.0f);
    glm::vec3 radiance(0.0f);
    float previous = 0.0f;
    for (int s = 0; s < kSteps; ++s) {
        const float x = (static_cast<float>(s) + 1.0f) / kSteps;
        const float next = t * x * x;
        const float dt = next - previous;
        const glm::vec3 pos = p + dir * (previous + 0.5f * dt);
        previous = next;
        const Medium m = medium_at(a, glm::length(pos) - a.ground_radius);
        const glm::vec3 extinction = glm::max(m.extinction, glm::vec3(1e-6f));
        const glm::vec3 step = exp3(-extinction * dt);
        const glm::vec3 sun_t =
            ray_sphere(pos, sun, a.ground_radius) > 0.0f ? glm::vec3(0.0f) : transmittance_at(a, pos, sun);
        const glm::vec3 source = (m.rayleigh * phase_r + m.mie * phase_m) * sun_t;
        radiance += throughput * (source - source * step) / extinction;
        throughput *= step;
    }
    return radiance;
}

glm::vec3 sky_illuminance(const AtmosphereParams& a, float height_km, const glm::vec3& sun) {
    F3D_ZONE;
    // Midpoint rule over the upper hemisphere: d(omega) = d(cos theta) d(phi).
    constexpr int kRings = 6;
    constexpr int kSegments = 12;
    glm::vec3 e(0.0f);
    const float d_cos = 1.0f / kRings;
    const float d_phi = 2.0f * kPi / kSegments;
    for (int i = 0; i < kRings; ++i) {
        const float cos_theta = (static_cast<float>(i) + 0.5f) * d_cos;
        const float sin_theta = std::sqrt(1.0f - cos_theta * cos_theta);
        for (int j = 0; j < kSegments; ++j) {
            const float phi = (static_cast<float>(j) + 0.5f) * d_phi;
            const glm::vec3 dir(sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi));
            e += single_scattering(a, height_km, dir, sun) * cos_theta * d_cos * d_phi;
        }
    }
    return e;
}

float exposure_from_ev100(float ev100) noexcept {
    return 1.0f / (1.2f * std::exp2(ev100));
}

float ev100_from_illuminance(float lux) noexcept {
    return std::log2(std::max(lux, 1e-6f) * 100.0f / 250.0f);
}

SkyLighting sky_lighting(const scene::Sky& sky, const scene::Celestial& celestial, const AtmosphereParams& a,
                         float camera_height_m) {
    F3D_ZONE;
    const float height_km = std::max(camera_height_m, 1.0f) / 1000.0f;
    SkyLighting out;
    out.sun_direction = celestial.sun_direction;
    out.moon_direction = celestial.moon_direction;
    const glm::vec3 sun_top(std::max(sky.sun_illuminance, 0.0f));
    out.sun_illuminance = sun_top * transmittance(a, height_km, celestial.sun_direction.y);
    if (sky.moon) {
        const float moon_top = kFullMoonIlluminance * celestial.moon_illumination;
        out.moon_illuminance = glm::vec3(moon_top) * transmittance(a, height_km, celestial.moon_direction.y);
        const float solid_angle = 2.0f * kPi * (1.0f - std::cos(kMoonAngularRadius));
        // The disc's brightness: the lit fraction spread over the whole disc.
        out.moon_radiance = glm::vec3(0.9f, 0.92f, 1.0f) * (moon_top / solid_angle);
    }
    // Airglow and starlight: about 0.001 lux.
    out.night_radiance = glm::vec3(0.6f, 0.8f, 1.2f) * 3.0e-4f;
    out.sky_illuminance = sky_illuminance(a, height_km, celestial.sun_direction) * sun_top + out.night_radiance * kPi;
    const glm::vec3 ground_irradiance = out.sun_illuminance * std::max(celestial.sun_direction.y, 0.0f) +
                                        out.moon_illuminance * std::max(celestial.moon_direction.y, 0.0f) +
                                        out.sky_illuminance;
    out.ground_radiance = a.ground_albedo / kPi * ground_irradiance;
    out.shadow_from_moon = sky.moon && celestial.sun_elevation < -0.05f && celestial.moon_elevation > 0.0f;

    // Incident-light metering of the horizontal illuminance (luminance weights),
    // clamped so night stays dark instead of being exposed like day.
    const float lux = glm::dot(ground_irradiance, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    out.ev100 = std::clamp(ev100_from_illuminance(lux), 1.0f, 16.0f) - sky.exposure_compensation;
    out.exposure = exposure_from_ev100(out.ev100);
    return out;
}

SkyConstants make_sky_constants(const AtmosphereParams& a, const scene::Sky& sky, const SkyLighting& lighting,
                                float camera_height_m) {
    SkyConstants c{};
    c.rayleigh_scattering = glm::vec4(a.rayleigh_scattering, a.rayleigh_scale_height);
    c.mie_scattering = glm::vec4(a.mie_scattering, a.mie_scale_height);
    c.mie_extinction = glm::vec4(a.mie_extinction, a.mie_g);
    c.ozone_absorption = glm::vec4(a.ozone_absorption, a.ozone_center);
    c.planet = glm::vec4(a.ground_radius, a.top_radius, a.ozone_half_width, std::max(camera_height_m, 1.0f) / 1000.0f);
    c.ground_albedo = glm::vec4(a.ground_albedo, 1.0f);
    c.sun_direction = glm::vec4(lighting.sun_direction, std::cos(kSunAngularRadius));
    c.sun_illuminance = glm::vec4(glm::vec3(std::max(sky.sun_illuminance, 0.0f)), 0.0f);
    c.ground_radiance = glm::vec4(lighting.ground_radiance, 0.0f);
    c.moon_direction = glm::vec4(lighting.moon_direction, std::cos(kMoonAngularRadius));
    c.moon_radiance = glm::vec4(lighting.moon_radiance, 0.0f);
    c.night_radiance = glm::vec4(lighting.night_radiance, 0.0f);
    return c;
}

SkyConstants make_hemisphere_constants(const glm::vec3& sky_radiance, const glm::vec3& ground_radiance) {
    SkyConstants c{};
    const AtmosphereParams a;
    c.planet = glm::vec4(a.ground_radius, a.top_radius, a.ozone_half_width, 0.001f);
    c.ground_albedo = glm::vec4(a.ground_albedo, 0.0f);
    c.sun_direction = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    c.moon_direction = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    c.hemi_sky = glm::vec4(sky_radiance, 0.0f);
    c.hemi_ground = glm::vec4(ground_radiance, 0.0f);
    return c;
}

} // namespace f3d::render::raster
