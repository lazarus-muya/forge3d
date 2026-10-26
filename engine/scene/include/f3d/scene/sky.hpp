// SPDX-License-Identifier: Apache-2.0
#pragma once

// Sun and moon positions for a Sky (phases.md P4.3).
//
// The sun follows the NOAA general solar position equations (about 0.5 degrees
// error); the moon follows Paul Schlyter's low-precision orbital elements without
// perturbation terms (about 1-2 degrees), which is plenty for lighting.
// Angles in radians unless a name says otherwise. World axes: +Y up, north -Z
// (rotated by Sky::north_offset), east +X.

#include "f3d/core/math.hpp"

#include <entt/entity/fwd.hpp>

namespace f3d::scene {

struct Sky;
class World;

/// A body's position in the local sky.
struct HorizontalPosition {
    float elevation = 0.0f; ///< above the horizon (negative: below)
    float azimuth = 0.0f;   ///< from north, clockwise seen from above (east = pi/2)
};

/// A date and local time.
struct SkyTime {
    int year = 2026;
    int month = 6;
    int day = 21;
    double hours = 12.0;     ///< local time
    double utc_offset = 0.0; ///< hours
};

[[nodiscard]] HorizontalPosition solar_position(const SkyTime& time, double latitude_deg, double longitude_deg);

struct LunarState {
    HorizontalPosition position;
    float illumination = 0.0f; ///< lit fraction of the disc (0 new moon, 1 full moon)
};
[[nodiscard]] LunarState lunar_state(const SkyTime& time, double latitude_deg, double longitude_deg);

/// Unit world-space direction towards a body (north_offset in degrees).
[[nodiscard]] glm::vec3 sky_direction(const HorizontalPosition& position, float north_offset_deg) noexcept;

/// Everything the renderer needs from a Sky's date, time and place.
struct Celestial {
    glm::vec3 sun_direction{0.0f, 1.0f, 0.0f}; ///< towards the sun
    float sun_elevation = 0.0f;
    glm::vec3 moon_direction{0.0f, 1.0f, 0.0f}; ///< towards the moon
    float moon_elevation = 0.0f;
    float moon_illumination = 0.0f;
};
[[nodiscard]] Celestial celestial(const Sky& sky);

/// The first visible entity with a Sky (scene order), or null.
[[nodiscard]] entt::entity active_sky(World& world);

} // namespace f3d::scene
