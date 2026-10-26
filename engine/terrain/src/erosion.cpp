// SPDX-License-Identifier: Apache-2.0
#include "f3d/terrain/erosion.hpp"

#include "f3d/core/jobs.hpp"
#include "f3d/core/profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace f3d::terrain {
namespace {

constexpr std::array<std::string_view, 3> kModeNames = {"hydraulic", "thermal", "both"};

constexpr float kGravity = 9.81f;
constexpr float kDt = 0.02f;     ///< simulation time step (cell units)
constexpr std::size_t kArrays = 11;

/// Uniform [0, 1) from (seed, iteration, cell): the rain pattern.
float hash01(std::uint32_t seed, std::uint32_t iteration, std::uint32_t cell) noexcept {
    std::uint32_t h = seed * 0x9E3779B1u ^ iteration * 0x85EBCA77u ^ cell * 0xC2B2AE3Du;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

void rows(std::uint32_t height, const std::function<void(std::uint32_t)>& row) {
    core::jobs::parallel_for(
        height,
        [&row](std::size_t begin, std::size_t end) {
            for (std::size_t z = begin; z < end; ++z) {
                row(static_cast<std::uint32_t>(z));
            }
        },
        8);
}

} // namespace

std::string_view to_string(ErosionMode mode) noexcept {
    return kModeNames[static_cast<std::size_t>(mode)];
}

bool parse(std::string_view text, ErosionMode& mode) noexcept {
    for (std::size_t i = 0; i < kModeNames.size(); ++i) {
        if (kModeNames[i] == text) {
            mode = static_cast<ErosionMode>(i);
            return true;
        }
    }
    return false;
}

std::size_t erosion_memory(std::uint32_t width, std::uint32_t height) noexcept {
    return static_cast<std::size_t>(width) * height * sizeof(float) * kArrays;
}

bool erode(std::span<float> heights, std::uint32_t width, std::uint32_t height, float spacing,
           const ErosionSettings& settings, float min_height, float max_height, const ErosionProgress& progress) {
    F3D_ZONE;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (width < 3 || height < 3 || heights.size() != count || !(spacing > 0.0f)) {
        return true; // nothing to do
    }
    // State, in cell units (heights / spacing): terrain b, water d, sediment s, outflow
    // flux per direction (to x-1, x+1, z-1, z+1); the thermal outflow of each cell and
    // that outflow per unit of excess (what a neighbour receives per unit of the excess
    // towards it); b2 and s2 receive each iteration.
    std::vector<float> b(count), b2(count), d(count, 0.0f), s(count, 0.0f), s2(count, 0.0f);
    std::vector<float> fl(count, 0.0f), fr(count, 0.0f), ft(count, 0.0f), fb(count, 0.0f);
    std::vector<float> out(count, 0.0f), out_per_excess(count, 0.0f);
    const float inv_spacing = 1.0f / spacing;
    for (std::size_t i = 0; i < count; ++i) {
        b[i] = heights[i] * inv_spacing;
    }

    const bool hydraulic = settings.mode != ErosionMode::Thermal;
    const float rain = std::max(settings.rain, 0.0f);
    const float evaporation = std::clamp(settings.evaporation, 0.0f, 1.0f);
    const float capacity = std::max(settings.capacity, 0.0f);
    const float dissolving = std::clamp(settings.dissolving, 0.0f, 1.0f);
    const float deposition = std::clamp(settings.deposition, 0.0f, 1.0f);
    const float min_slope = std::clamp(settings.min_slope, 0.0f, 1.0f);
    const float talus = std::tan(std::clamp(settings.talus_angle, 0.0f, 89.0f) * 0.017453293f);
    const float thermal_rate = std::clamp(settings.thermal_rate, 0.0f, 1.0f);
    const bool thermal = settings.mode != ErosionMode::Hydraulic && thermal_rate > 0.0f;
    const std::uint32_t w = width;
    const std::uint32_t h = height;
    const auto at = [w](std::uint32_t x, std::uint32_t z) { return static_cast<std::size_t>(z) * w + x; };

    // Neighbours for the thermal step (4 sides, then 4 diagonals).
    constexpr std::array<int, 8> dx = {-1, 1, 0, 0, -1, 1, -1, 1};
    constexpr std::array<int, 8> dz = {0, 0, -1, 1, -1, -1, 1, 1};
    constexpr std::array<float, 8> dist = {1.0f, 1.0f, 1.0f, 1.0f, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f};
    const auto sw = static_cast<std::ptrdiff_t>(w);
    const std::array<std::ptrdiff_t, 8> offset = {-1, 1, -sw, sw, -sw - 1, -sw + 1, sw - 1, sw + 1};
    const auto inside = [&](std::uint32_t x, std::uint32_t z, int k) {
        const int nx = static_cast<int>(x) + dx[k];
        const int nz = static_cast<int>(z) + dz[k];
        return nx >= 0 && nz >= 0 && nx < static_cast<int>(w) && nz < static_cast<int>(h);
    };
    const auto neighbour = [&](std::size_t i, int k) {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) + offset[k]);
    };

    // Rain for an iteration (a hash of the seed, the iteration and the cell); each
    // iteration's rain is added at the end of the previous one.
    const auto rain_at = [&](std::uint32_t iteration, std::size_t i) {
        return rain * (0.5f + hash01(settings.seed, iteration, static_cast<std::uint32_t>(i)));
    };
    if (hydraulic && settings.iterations > 0) {
        for (std::size_t i = 0; i < count; ++i) {
            d[i] = rain_at(0, i);
        }
    }

    // One iteration; the modes are template parameters so the inner loops carry no
    // per-cell mode branches.
    const auto step = [&]<bool kHydraulic, bool kThermal>(std::uint32_t iteration) {
        const bool more = iteration + 1 < settings.iterations;
        // Pass 1 (reads b and d; writes the fluxes and the thermal outflow):
        // - outflow flux from the water surface differences, scaled so a cell never sends
        //   more water than it has;
        // - thermal: a share of the steepest excess over the talus slope leaves the cell.
        rows(h, [&](std::uint32_t z) {
            const bool edge_row = z == 0 || z + 1 == h;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = at(x, z);
                const bool edge = edge_row || x == 0 || x + 1 == w;
                if constexpr (kHydraulic) {
                    const float water = d[i];
                    const float surface = b[i] + water;
                    const auto flow = [&](float flux, bool open, std::size_t n) {
                        return open ? std::max(0.0f, flux + kDt * kGravity * (surface - b[n] - d[n])) : 0.0f;
                    };
                    float l = flow(fl[i], x > 0, x > 0 ? i - 1 : i);
                    float r = flow(fr[i], x + 1 < w, x + 1 < w ? i + 1 : i);
                    float t = flow(ft[i], z > 0, z > 0 ? i - w : i);
                    float bt = flow(fb[i], z + 1 < h, z + 1 < h ? i + w : i);
                    const float total = (l + r + t + bt) * kDt;
                    if (total > water && total > 0.0f) {
                        const float k = water / total;
                        l *= k;
                        r *= k;
                        t *= k;
                        bt *= k;
                    }
                    fl[i] = l;
                    fr[i] = r;
                    ft[i] = t;
                    fb[i] = bt;
                }
                if constexpr (kThermal) {
                    float sum = 0.0f;
                    float most = 0.0f;
                    for (int k = 0; k < 8; ++k) {
                        if (edge && !inside(x, z, k)) {
                            continue;
                        }
                        const float excess = b[i] - b[neighbour(i, k)] - talus * dist[k];
                        if (excess > 0.0f) {
                            sum += excess;
                            most = std::max(most, excess);
                        }
                    }
                    out[i] = thermal_rate * most * 0.5f;
                    out_per_excess[i] = sum > 0.0f ? out[i] / sum : 0.0f;
                }
            }
        });
        // Pass 2 (reads the old b and s and pass 1's results; each cell writes only its own
        // b2, s2 and d): water update and velocity; sediment carried in from upstream
        // (semi-Lagrangian); erosion / deposition; the thermal exchange; evaporation and
        // the next iteration's rain.
        rows(h, [&](std::uint32_t z) {
            const bool edge_row = z == 0 || z + 1 == h;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = at(x, z);
                const bool edge = edge_row || x == 0 || x + 1 == w;
                float terrain = b[i];
                float sediment = s[i];
                if constexpr (kHydraulic) {
                    const float in_l = x > 0 ? fr[i - 1] : 0.0f;
                    const float in_r = x + 1 < w ? fl[i + 1] : 0.0f;
                    const float in_t = z > 0 ? fb[i - w] : 0.0f;
                    const float in_b = z + 1 < h ? ft[i + w] : 0.0f;
                    const float d1 = d[i];
                    const float d2 =
                        std::max(0.0f, d1 + kDt * (in_l + in_r + in_t + in_b - fl[i] - fr[i] - ft[i] - fb[i]));
                    const float mean = 0.5f * (d1 + d2);
                    float u = 0.0f;
                    float v = 0.0f;
                    if (mean > 1e-4f) {
                        u = 0.5f * (in_l - fl[i] + fr[i] - in_r) / mean;
                        v = 0.5f * (in_t - ft[i] + fb[i] - in_b) / mean;
                    }
                    // Sediment arriving from upstream.
                    const float px = std::clamp(static_cast<float>(x) - u * kDt, 0.0f, static_cast<float>(w - 1));
                    const float pz = std::clamp(static_cast<float>(z) - v * kDt, 0.0f, static_cast<float>(h - 1));
                    const auto x0 = std::min(static_cast<std::uint32_t>(px), w - 2);
                    const auto z0 = std::min(static_cast<std::uint32_t>(pz), h - 2);
                    const float tx = px - static_cast<float>(x0);
                    const float tz = pz - static_cast<float>(z0);
                    const float a = s[at(x0, z0)] + (s[at(x0 + 1, z0)] - s[at(x0, z0)]) * tx;
                    const float c = s[at(x0, z0 + 1)] + (s[at(x0 + 1, z0 + 1)] - s[at(x0, z0 + 1)]) * tx;
                    sediment = a + (c - a) * tz;
                    // Pick up or drop soil towards what the water can carry here.
                    const float gx = (b[at(std::min(x + 1, w - 1), z)] - b[at(x > 0 ? x - 1 : 0, z)]) * 0.5f;
                    const float gz = (b[at(x, std::min(z + 1, h - 1))] - b[at(x, z > 0 ? z - 1 : 0)]) * 0.5f;
                    const float g2 = gx * gx + gz * gz;
                    const float tilt = std::max(std::sqrt(g2 / (1.0f + g2)), min_slope);
                    const float carry = capacity * tilt * std::sqrt(u * u + v * v) * std::min(mean * 4.0f, 1.0f);
                    const float amount = carry > sediment ? dissolving * (carry - sediment) * kDt
                                                          : -deposition * (sediment - carry) * kDt;
                    terrain -= amount;
                    sediment += amount;
                    d[i] = d2 * (1.0f - evaporation) + (more ? rain_at(iteration + 1, i) : 0.0f);
                }
                if constexpr (kThermal) {
                    float received = 0.0f;
                    for (int k = 0; k < 8; ++k) {
                        if (edge && !inside(x, z, k)) {
                            continue;
                        }
                        const std::size_t n = neighbour(i, k);
                        const float excess = b[n] - b[i] - talus * dist[k]; // how far the neighbour is above our talus
                        if (excess > 0.0f) {
                            received += out_per_excess[n] * excess;
                        }
                    }
                    terrain += received - out[i];
                }
                b2[i] = terrain;
                s2[i] = sediment;
            }
        });
        std::swap(b, b2);
        std::swap(s, s2);
    };
    for (std::uint32_t iteration = 0; iteration < settings.iterations; ++iteration) {
        if (hydraulic && thermal) {
            step.template operator()<true, true>(iteration);
        } else if (hydraulic) {
            step.template operator()<true, false>(iteration);
        } else if (thermal) {
            step.template operator()<false, true>(iteration);
        }
        if (progress && !progress(static_cast<float>(iteration + 1) / static_cast<float>(settings.iterations))) {
            return false;
        }
    }
    // Whatever is still suspended settles where it is.
    for (std::size_t i = 0; i < count; ++i) {
        heights[i] = std::clamp((b[i] + s[i]) * spacing, min_height, max_height);
    }
    return true;
}

} // namespace f3d::terrain
