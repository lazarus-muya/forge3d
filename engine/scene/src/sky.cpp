// SPDX-License-Identifier: Apache-2.0
#include "f3d/scene/sky.hpp"

#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace f3d::scene {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;

bool is_leap(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int day_of_year(int year, int month, int day) noexcept {
    static constexpr int kCumulative[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int m = std::clamp(month, 1, 12);
    return kCumulative[m - 1] + day + (m > 2 && is_leap(year) ? 1 : 0);
}

/// Elevation and azimuth from declination, hour angle and latitude (radians).
HorizontalPosition to_horizontal(double declination, double hour_angle, double latitude) noexcept {
    const double sin_elevation =
        std::sin(latitude) * std::sin(declination) + std::cos(latitude) * std::cos(declination) * std::cos(hour_angle);
    HorizontalPosition out;
    out.elevation = static_cast<float>(std::asin(std::clamp(sin_elevation, -1.0, 1.0)));
    // Measured from south, westward; +pi turns it into north, clockwise.
    const double azimuth = std::atan2(std::sin(hour_angle), std::cos(hour_angle) * std::sin(latitude) -
                                                                std::tan(declination) * std::cos(latitude)) +
                           kPi;
    out.azimuth = static_cast<float>(std::fmod(azimuth + 2.0 * kPi, 2.0 * kPi));
    return out;
}

double wrap_degrees(double degrees) noexcept {
    degrees = std::fmod(degrees, 360.0);
    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

} // namespace

HorizontalPosition solar_position(const SkyTime& time, double latitude_deg, double longitude_deg) {
    const double utc_hours = time.hours - time.utc_offset;
    const double days = is_leap(time.year) ? 366.0 : 365.0;
    const double gamma =
        2.0 * kPi / days * (day_of_year(time.year, time.month, time.day) - 1 + (utc_hours - 12.0) / 24.0);
    const double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) -
                                    0.014615 * std::cos(2.0 * gamma) - 0.040849 * std::sin(2.0 * gamma));
    const double declination = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) -
                               0.006758 * std::cos(2.0 * gamma) + 0.000907 * std::sin(2.0 * gamma) -
                               0.002697 * std::cos(3.0 * gamma) + 0.00148 * std::sin(3.0 * gamma);
    const double time_offset = eqtime + 4.0 * longitude_deg - 60.0 * time.utc_offset; // minutes
    const double true_solar_minutes = time.hours * 60.0 + time_offset;
    const double hour_angle = (true_solar_minutes / 4.0 - 180.0) * kDeg;
    return to_horizontal(declination, hour_angle, latitude_deg * kDeg);
}

LunarState lunar_state(const SkyTime& time, double latitude_deg, double longitude_deg) {
    // Days since 2000 Jan 0.0 UT (Schlyter, "How to compute planetary positions").
    const int y = time.year;
    const int m = time.month;
    const double ut = time.hours - time.utc_offset;
    const double d = 367.0 * y - (7 * (y + (m + 9) / 12)) / 4 + (275 * m) / 9 + time.day - 730530 + ut / 24.0;

    // Sun (for the sidereal time and the moon's phase).
    const double ws = 282.9404 + 4.70935e-5 * d;
    const double ms = wrap_degrees(356.0470 + 0.9856002585 * d);
    const double es = 0.016709 - 1.151e-9 * d;
    const double ecl = (23.4393 - 3.563e-7 * d) * kDeg;
    const double ea_s = ms * kDeg + es * std::sin(ms * kDeg) * (1.0 + es * std::cos(ms * kDeg));
    const double xs = std::cos(ea_s) - es;
    const double ys = std::sqrt(1.0 - es * es) * std::sin(ea_s);
    const double sun_longitude = std::atan2(ys, xs) + ws * kDeg;

    // Moon's orbital elements.
    const double n = wrap_degrees(125.1228 - 0.0529538083 * d) * kDeg;
    const double i = 5.1454 * kDeg;
    const double w = wrap_degrees(318.0634 + 0.1643573223 * d) * kDeg;
    const double e = 0.054900;
    const double mm = wrap_degrees(115.3654 + 13.0649929509 * d) * kDeg;
    double ea = mm + e * std::sin(mm) * (1.0 + e * std::cos(mm));
    for (int iteration = 0; iteration < 5; ++iteration) {
        ea -= (ea - e * std::sin(ea) - mm) / (1.0 - e * std::cos(ea));
    }
    const double xv = std::cos(ea) - e;
    const double yv = std::sqrt(1.0 - e * e) * std::sin(ea);
    const double v = std::atan2(yv, xv);
    const double xh = std::cos(n) * std::cos(v + w) - std::sin(n) * std::sin(v + w) * std::cos(i);
    const double yh = std::sin(n) * std::cos(v + w) + std::cos(n) * std::sin(v + w) * std::cos(i);
    const double zh = std::sin(v + w) * std::sin(i);
    const double lon = std::atan2(yh, xh);
    const double lat = std::atan2(zh, std::sqrt(xh * xh + yh * yh));

    // Ecliptic -> equatorial.
    const double xe = std::cos(lat) * std::cos(lon);
    const double ye = std::cos(ecl) * std::cos(lat) * std::sin(lon) - std::sin(ecl) * std::sin(lat);
    const double ze = std::sin(ecl) * std::cos(lat) * std::sin(lon) + std::cos(ecl) * std::sin(lat);
    const double ra = std::atan2(ye, xe);
    const double declination = std::atan2(ze, std::sqrt(xe * xe + ye * ye));

    // Local sidereal time (degrees) -> hour angle.
    const double gmst0 = ms + ws + 180.0;
    const double lst = gmst0 + ut * 15.0 + longitude_deg;
    const double hour_angle = (wrap_degrees(lst - ra / kDeg + 180.0) - 180.0) * kDeg;

    LunarState out;
    out.position = to_horizontal(declination, hour_angle, latitude_deg * kDeg);
    const double elongation = std::acos(std::clamp(std::cos(sun_longitude - lon) * std::cos(lat), -1.0, 1.0));
    out.illumination = static_cast<float>((1.0 - std::cos(elongation)) * 0.5);
    return out;
}

glm::vec3 sky_direction(const HorizontalPosition& position, float north_offset_deg) noexcept {
    const float azimuth = position.azimuth + north_offset_deg * static_cast<float>(kDeg);
    const float c = std::cos(position.elevation);
    return glm::normalize(glm::vec3(std::sin(azimuth) * c, std::sin(position.elevation), -std::cos(azimuth) * c));
}

Celestial celestial(const Sky& sky) {
    const SkyTime time{sky.year, sky.month, sky.day, sky.time_of_day, sky.utc_offset};
    const HorizontalPosition sun = solar_position(time, sky.latitude, sky.longitude);
    const LunarState moon = lunar_state(time, sky.latitude, sky.longitude);
    Celestial out;
    out.sun_direction = sky_direction(sun, sky.north_offset);
    out.sun_elevation = sun.elevation;
    out.moon_direction = sky_direction(moon.position, sky.north_offset);
    out.moon_elevation = moon.position.elevation;
    out.moon_illumination = moon.illumination;
    return out;
}

entt::entity active_sky(World& world) {
    const auto& registry = world.registry();
    for (const entt::entity e : world.depth_first()) {
        if (registry.all_of<Sky>(e) && is_visible(world, e)) {
            return e;
        }
    }
    return entt::null;
}

} // namespace f3d::scene
