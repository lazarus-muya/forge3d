// SPDX-License-Identifier: Apache-2.0
#include "f3d/terrain/builtin_textures.hpp"

#include "f3d/core/jobs.hpp"
#include "f3d/core/profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>

namespace f3d::terrain {
namespace {

constexpr float kTau = 6.2831853f;

std::uint32_t hash3(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^
                      seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

float rnd(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept {
    return static_cast<float>(hash3(x, y, seed) >> 8) / 16777216.0f;
}

std::int32_t wrap(std::int32_t i, std::int32_t period) noexcept {
    return ((i % period) + period) % period;
}

/// Gradient noise periodic over [0, period_x) x [0, period_y), about [-0.7, 0.7].
float pnoise(float x, float y, std::int32_t period_x, std::int32_t period_y, std::uint32_t seed) noexcept {
    const float fx0 = std::floor(x);
    const float fy0 = std::floor(y);
    const auto ix = static_cast<std::int32_t>(fx0);
    const auto iy = static_cast<std::int32_t>(fy0);
    const float dx = x - fx0;
    const float dy = y - fy0;
    const auto grad = [&](std::int32_t gx, std::int32_t gy, float ox, float oy) {
        const float a = rnd(wrap(gx, period_x), wrap(gy, period_y), seed) * kTau;
        return std::cos(a) * ox + std::sin(a) * oy;
    };
    const auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float u = fade(dx);
    const float v = fade(dy);
    const float a = grad(ix, iy, dx, dy);
    const float b = grad(ix + 1, iy, dx - 1.0f, dy);
    const float c = grad(ix, iy + 1, dx, dy - 1.0f);
    const float d = grad(ix + 1, iy + 1, dx - 1.0f, dy - 1.0f);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

float pnoise(float x, float y, std::int32_t period, std::uint32_t seed) noexcept {
    return pnoise(x, y, period, period, seed);
}

/// Tileable fBm over the unit square, about [-1, 1].
float fbm(float u, float v, std::int32_t period, int octaves, std::uint32_t seed, float persistence = 0.5f) noexcept {
    float sum = 0.0f;
    float amp = 1.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const std::int32_t p = period << o;
        sum += pnoise(u * static_cast<float>(p), v * static_cast<float>(p), p, seed + static_cast<std::uint32_t>(o)) *
               amp;
        norm += amp;
        amp *= persistence;
    }
    return sum / norm * 1.6f;
}

/// Ridged multifractal in [0, 1] (sharp crests).
float ridged(float u, float v, std::int32_t period, int octaves, std::uint32_t seed) noexcept {
    float sum = 0.0f;
    float amp = 0.5f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const std::int32_t p = period << o;
        const float n = 1.0f - std::abs(pnoise(u * static_cast<float>(p), v * static_cast<float>(p), p,
                                               seed + static_cast<std::uint32_t>(o)) *
                                        1.4f);
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5f;
    }
    return std::clamp(sum / norm, 0.0f, 1.0f);
}

struct Cell {
    float f1 = 1e9f; ///< distance to the nearest feature point (cell units)
    float f2 = 1e9f; ///< to the second nearest
    std::uint32_t id = 0;
};

/// Tileable Worley noise: `cells` per side.
Cell voronoi(float u, float v, std::int32_t cells, std::uint32_t seed, float jitter = 0.9f) noexcept {
    const float x = u * static_cast<float>(cells);
    const float y = v * static_cast<float>(cells);
    const auto ix = static_cast<std::int32_t>(std::floor(x));
    const auto iy = static_cast<std::int32_t>(std::floor(y));
    Cell out;
    for (std::int32_t oy = -1; oy <= 1; ++oy) {
        for (std::int32_t ox = -1; ox <= 1; ++ox) {
            const std::int32_t cx = wrap(ix + ox, cells);
            const std::int32_t cy = wrap(iy + oy, cells);
            const float px = static_cast<float>(ix + ox) + 0.5f + (rnd(cx, cy, seed) - 0.5f) * jitter;
            const float py = static_cast<float>(iy + oy) + 0.5f + (rnd(cx, cy, seed + 7u) - 0.5f) * jitter;
            const float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < out.f1) {
                out.f2 = out.f1;
                out.f1 = d;
                out.id = hash3(cx, cy, seed + 13u);
            } else if (d < out.f2) {
                out.f2 = d;
            }
        }
    }
    return out;
}

float smooth(float e0, float e1, float x) noexcept {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

glm::vec3 mix(const glm::vec3& a, const glm::vec3& b, float t) noexcept {
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

float unit(std::uint32_t id, std::uint32_t salt) noexcept {
    return static_cast<float>(hash3(static_cast<std::int32_t>(id), static_cast<std::int32_t>(salt), 99u) >> 8) /
           16777216.0f;
}

/// One texel of a surface: height (0..1), linear albedo, roughness.
struct Texel {
    float height = 0.5f;
    glm::vec3 albedo{0.5f};
    float roughness = 0.8f;
};
using SurfaceFn = std::function<Texel(float u, float v)>;

struct SurfaceDef {
    BuiltinSurface info;
    float normal_strength; ///< height (0..1) to texels
    SurfaceFn sample;
};

Texel grass(float u, float v, bool meadow) {
    // Blades: noise stretched along a wavering direction, over clumps.
    const float clumps = fbm(u, v, 4, 4, 11u) * 0.5f + 0.5f;
    const float warp = fbm(u, v, 2, 2, 12u) * 0.03f;
    const float blades = pnoise((u + warp) * 192.0f, v * 48.0f, 192, 48, 13u) * 0.5f + 0.5f;
    const float fine = fbm(u, v, 64, 3, 14u) * 0.5f + 0.5f;
    Texel t;
    t.height = std::clamp(blades * 0.55f + clumps * 0.3f + fine * 0.15f, 0.0f, 1.0f);
    const float dry = smooth(0.25f, 0.75f, fbm(u, v, 3, 3, 15u) * 0.5f + 0.5f);
    const glm::vec3 dark = meadow ? glm::vec3(0.035f, 0.075f, 0.012f) : glm::vec3(0.018f, 0.040f, 0.008f);
    const glm::vec3 light = meadow ? glm::vec3(0.17f, 0.23f, 0.05f) : glm::vec3(0.11f, 0.16f, 0.03f);
    const glm::vec3 straw(0.20f, 0.17f, 0.07f);
    t.albedo = mix(mix(dark, light, t.height), straw, dry * (meadow ? 0.25f : 0.45f) * fine);
    if (meadow) { // flowers
        const Cell c = voronoi(u, v, 96, 16u);
        if (c.f1 < 0.12f && unit(c.id, 1) < 0.35f) {
            const float pick = unit(c.id, 2);
            t.albedo = pick < 0.5f ? glm::vec3(0.75f, 0.62f, 0.08f)
                                   : (pick < 0.8f ? glm::vec3(0.8f, 0.8f, 0.78f) : glm::vec3(0.45f, 0.15f, 0.55f));
            t.height = 0.95f;
        }
    }
    t.roughness = 0.88f + 0.08f * (1.0f - t.height);
    return t;
}

Texel forest_floor(float u, float v) {
    const float base = fbm(u, v, 4, 5, 21u) * 0.5f + 0.5f;
    const Cell leaf = voronoi(u, v, 40, 22u, 1.0f);
    const float leaf_shape = 1.0f - smooth(0.25f, 0.55f, leaf.f1);
    const float hue = unit(leaf.id, 3);
    const glm::vec3 soil = mix(glm::vec3(0.035f, 0.025f, 0.015f), glm::vec3(0.09f, 0.065f, 0.035f), base);
    const glm::vec3 leaf_col = hue < 0.4f   ? glm::vec3(0.22f, 0.10f, 0.03f)
                               : hue < 0.75f ? glm::vec3(0.14f, 0.08f, 0.03f)
                                             : glm::vec3(0.07f, 0.08f, 0.02f);
    const float moss = smooth(0.55f, 0.8f, fbm(u, v, 3, 3, 23u) * 0.5f + 0.5f);
    Texel t;
    const bool on_leaf = leaf_shape > 0.3f && unit(leaf.id, 4) < 0.7f;
    t.height = std::clamp(base * 0.4f + (on_leaf ? 0.4f + leaf_shape * 0.2f : 0.0f), 0.0f, 1.0f);
    t.albedo = mix(on_leaf ? leaf_col * (0.8f + 0.4f * unit(leaf.id, 5)) : soil, glm::vec3(0.05f, 0.09f, 0.02f),
                   moss * 0.7f);
    t.roughness = 0.9f;
    return t;
}

Texel dirt(float u, float v, bool wet) {
    const float base = fbm(u, v, 4, 6, 31u) * 0.5f + 0.5f;
    const Cell pebble = voronoi(u, v, 48, 32u);
    const float stone = unit(pebble.id, 6) < 0.3f ? 1.0f - smooth(0.2f, 0.45f, pebble.f1) : 0.0f;
    Texel t;
    t.height = std::clamp(base * 0.7f + stone * 0.35f, 0.0f, 1.0f);
    if (wet) {
        t.albedo = mix(glm::vec3(0.035f, 0.025f, 0.015f), glm::vec3(0.08f, 0.06f, 0.035f), base);
        const float puddle = 1.0f - smooth(0.3f, 0.45f, base);
        t.roughness = 0.75f - puddle * 0.55f;
        t.height = std::max(t.height, 0.3f);
    } else {
        t.albedo = mix(glm::vec3(0.09f, 0.06f, 0.035f), glm::vec3(0.19f, 0.14f, 0.09f), base);
        t.albedo = mix(t.albedo, glm::vec3(0.22f, 0.20f, 0.17f) * (0.7f + 0.6f * unit(pebble.id, 7)), stone);
        t.roughness = 0.92f - stone * 0.15f;
    }
    return t;
}

Texel rock(float u, float v, bool cliff) {
    const float r = ridged(u, v, 3, 6, 41u);
    const float n = fbm(u, v, 6, 5, 42u) * 0.5f + 0.5f;
    // A few irregular cracks: Worley edges under a strong domain warp.
    const float wu = u + fbm(u, v, 3, 3, 43u) * 0.08f;
    const float wv = v + fbm(u, v, 3, 3, 47u) * 0.08f;
    const Cell crack = voronoi(wu, wv, 4, 44u);
    const float cracks = smooth(0.0f, 0.035f, crack.f2 - crack.f1);
    const float faces = unit(crack.id, 11); // each plate a little different
    Texel t;
    if (cliff) { // horizontal strata
        const float strata = std::sin((v * 14.0f + fbm(u, v, 2, 3, 45u) * 0.8f) * kTau) * 0.5f + 0.5f;
        t.height = std::clamp((r * 0.5f + strata * 0.35f + n * 0.15f) * (0.7f + 0.3f * cracks), 0.0f, 1.0f);
        t.albedo = mix(glm::vec3(0.11f, 0.085f, 0.065f), glm::vec3(0.30f, 0.24f, 0.18f), strata * 0.6f + n * 0.4f);
    } else {
        t.height = std::clamp((r * 0.6f + n * 0.3f + faces * 0.1f) * (0.75f + 0.25f * cracks), 0.0f, 1.0f);
        t.albedo = mix(glm::vec3(0.09f, 0.085f, 0.075f), glm::vec3(0.27f, 0.245f, 0.21f),
                       n * 0.6f + r * 0.25f + faces * 0.15f);
    }
    t.albedo *= 0.7f + 0.3f * cracks;
    const float lichen = smooth(0.62f, 0.75f, fbm(u, v, 8, 3, 46u) * 0.5f + 0.5f);
    t.albedo = mix(t.albedo, glm::vec3(0.16f, 0.17f, 0.08f), lichen * 0.45f);
    t.roughness = 0.8f + 0.15f * (1.0f - t.height);
    return t;
}

Texel sand(float u, float v) {
    const float warp = fbm(u, v, 2, 3, 51u);
    const float ripples = std::sin((v * 24.0f + warp * 2.0f) * kTau) * 0.5f + 0.5f;
    const float grains = fbm(u, v, 128, 2, 52u) * 0.5f + 0.5f;
    const float dunes = fbm(u, v, 2, 4, 53u) * 0.5f + 0.5f;
    Texel t;
    t.height = std::clamp(ripples * 0.45f + dunes * 0.35f + grains * 0.2f, 0.0f, 1.0f);
    t.albedo = mix(glm::vec3(0.36f, 0.28f, 0.17f), glm::vec3(0.55f, 0.45f, 0.30f), dunes * 0.6f + grains * 0.4f);
    t.roughness = 0.85f;
    return t;
}

Texel gravel(float u, float v) {
    const Cell c = voronoi(u, v, 28, 61u);
    const Cell small = voronoi(u, v, 70, 62u);
    const float big = std::clamp(1.0f - c.f1 * 1.5f, 0.0f, 1.0f);
    const float little = std::clamp(1.0f - small.f1 * 1.6f, 0.0f, 1.0f);
    Texel t;
    const bool top = big * 0.9f > little * 0.6f;
    const float shape = top ? std::sqrt(big) : std::sqrt(little) * 0.65f;
    t.height = std::clamp(shape + (fbm(u, v, 32, 2, 63u) * 0.05f), 0.0f, 1.0f);
    const float tone = unit(top ? c.id : small.id, 8);
    t.albedo = mix(glm::vec3(0.10f, 0.095f, 0.09f), glm::vec3(0.38f, 0.35f, 0.31f), tone) * (0.5f + 0.5f * t.height);
    t.roughness = 0.75f + 0.2f * (1.0f - t.height);
    return t;
}

Texel snow(float u, float v) {
    const float n = fbm(u, v, 3, 5, 71u) * 0.5f + 0.5f;
    const float sparkle = fbm(u, v, 128, 1, 72u);
    Texel t;
    t.height = n;
    t.albedo = mix(glm::vec3(0.70f, 0.74f, 0.80f), glm::vec3(0.88f, 0.90f, 0.93f), n);
    t.roughness = std::clamp(0.62f - std::max(sparkle - 0.5f, 0.0f) * 0.6f, 0.2f, 1.0f);
    return t;
}

Texel asphalt(float u, float v) {
    const float n = fbm(u, v, 8, 4, 81u) * 0.5f + 0.5f;
    const float speck = fbm(u, v, 256, 1, 82u);
    const float stones = speck > 0.45f ? 1.0f : 0.0f;
    const float wear = fbm(u, v, 2, 3, 83u) * 0.5f + 0.5f;
    Texel t;
    t.height = std::clamp(n * 0.6f + stones * 0.3f, 0.0f, 1.0f);
    t.albedo = mix(glm::vec3(0.025f, 0.025f, 0.025f), glm::vec3(0.06f, 0.06f, 0.058f), n * 0.5f + wear * 0.5f);
    t.albedo = mix(t.albedo, glm::vec3(0.14f, 0.13f, 0.12f), stones * 0.6f);
    t.roughness = 0.85f - stones * 0.1f;
    return t;
}

Texel cobblestone(float u, float v) {
    const Cell c = voronoi(u, v, 10, 91u, 0.6f);
    const float gap = c.f2 - c.f1;
    const float stone = smooth(0.04f, 0.16f, gap);
    const float n = fbm(u, v, 16, 3, 92u) * 0.5f + 0.5f;
    Texel t;
    t.height = std::clamp(stone * (0.75f + 0.25f * n) * (0.85f + 0.15f * unit(c.id, 9)), 0.0f, 1.0f);
    const glm::vec3 col = mix(glm::vec3(0.12f, 0.11f, 0.10f), glm::vec3(0.30f, 0.27f, 0.23f), unit(c.id, 10));
    t.albedo = mix(glm::vec3(0.06f, 0.055f, 0.045f), col * (0.8f + 0.4f * n), stone);
    t.roughness = 0.7f + 0.25f * (1.0f - stone);
    return t;
}

const std::vector<SurfaceDef>& surfaces() {
    static const std::vector<SurfaceDef> defs = {
        {{"grass", "Grass", 3.0f, false}, 3.0f, [](float u, float v) { return grass(u, v, false); }},
        {{"meadow", "Meadow", 3.0f, false}, 3.0f, [](float u, float v) { return grass(u, v, true); }},
        {{"forest_floor", "Forest floor", 3.0f, false}, 4.0f, forest_floor},
        {{"dirt", "Dirt", 3.0f, false}, 4.0f, [](float u, float v) { return dirt(u, v, false); }},
        {{"mud", "Mud", 4.0f, false}, 3.0f, [](float u, float v) { return dirt(u, v, true); }},
        {{"rock", "Rock", 6.0f, true}, 8.0f, [](float u, float v) { return rock(u, v, false); }},
        {{"cliff", "Cliff", 12.0f, true}, 10.0f, [](float u, float v) { return rock(u, v, true); }},
        {{"sand", "Sand", 4.0f, false}, 2.0f, sand},
        {{"gravel", "Gravel", 2.0f, false}, 6.0f, gravel},
        {{"snow", "Snow", 6.0f, false}, 2.0f, snow},
        {{"asphalt", "Asphalt", 3.0f, false}, 2.0f, asphalt},
        {{"cobblestone", "Cobblestone", 3.0f, false}, 7.0f, cobblestone},
    };
    return defs;
}

struct GeneratedSet {
    std::array<core::Image, kLayerTextureSlots> images; ///< by LayerTextureSlot (Ao unused)
};

GeneratedSet generate(const SurfaceDef& def) {
    F3D_ZONE;
    const std::uint32_t n = kBuiltinTextureSize;
    std::vector<Texel> texels(static_cast<std::size_t>(n) * n);
    core::jobs::parallel_for(
        n,
        [&](std::size_t begin, std::size_t end) {
            for (auto y = static_cast<std::uint32_t>(begin); y < end; ++y) {
                for (std::uint32_t x = 0; x < n; ++x) {
                    texels[static_cast<std::size_t>(y) * n + x] =
                        def.sample((static_cast<float>(x) + 0.5f) / static_cast<float>(n),
                                   (static_cast<float>(y) + 0.5f) / static_cast<float>(n));
                }
            }
        },
        8);
    GeneratedSet set;
    for (core::Image& image : set.images) {
        image.width = image.height = n;
        image.rgba.assign(static_cast<std::size_t>(n) * n * 4, 255);
    }
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const auto h = [&](std::int32_t x, std::int32_t y) {
        return texels[static_cast<std::size_t>(wrap(y, static_cast<std::int32_t>(n))) * n +
                      static_cast<std::size_t>(wrap(x, static_cast<std::int32_t>(n)))]
            .height;
    };
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * n + x) * 4;
            const Texel& t = texels[static_cast<std::size_t>(y) * n + x];
            for (int c = 0; c < 3; ++c) {
                set.images[0].rgba[i + c] = byte(core::linear_to_srgb(t.albedo[c]));
            }
            // OpenGL-style normal: red right (+x), green up the image (-y rows).
            const auto ix = static_cast<std::int32_t>(x);
            const auto iy = static_cast<std::int32_t>(y);
            const float dx = (h(ix + 1, iy) - h(ix - 1, iy)) * 0.5f * def.normal_strength;
            const float dy = (h(ix, iy + 1) - h(ix, iy - 1)) * 0.5f * def.normal_strength;
            const glm::vec3 normal = glm::normalize(glm::vec3(-dx, dy, 1.0f));
            for (int c = 0; c < 3; ++c) {
                set.images[1].rgba[i + c] = byte(normal[c] * 0.5f + 0.5f);
                set.images[2].rgba[i + c] = byte(t.roughness);
                set.images[4].rgba[i + c] = byte(t.height);
            }
        }
    }
    set.images[3] = {}; // no AO map: the height already shades the colour
    return set;
}

constexpr std::array<const char*, kLayerTextureSlots> kSlotNames = {"albedo", "normal", "roughness", "ao", "height"};

} // namespace

const std::vector<BuiltinSurface>& builtin_surfaces() {
    static const std::vector<BuiltinSurface> out = [] {
        std::vector<BuiltinSurface> list;
        for (const SurfaceDef& def : surfaces()) {
            list.push_back(def.info);
        }
        return list;
    }();
    return out;
}

std::string builtin_texture_path(std::string_view id, LayerTextureSlot slot) {
    return std::string(kBuiltinTexturePrefix) + std::string(id) + "/" + kSlotNames[static_cast<std::size_t>(slot)];
}

std::optional<core::Image> builtin_texture(std::string_view path) {
    if (!path.starts_with(kBuiltinTexturePrefix)) {
        return std::nullopt;
    }
    const std::string_view rest = path.substr(kBuiltinTexturePrefix.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view id = rest.substr(0, slash);
    const std::string_view slot_name = rest.substr(slash + 1);
    const auto slot_it = std::find(kSlotNames.begin(), kSlotNames.end(), slot_name);
    const auto def_it = std::find_if(surfaces().begin(), surfaces().end(),
                                     [&](const SurfaceDef& def) { return id == def.info.id; });
    if (slot_it == kSlotNames.end() || def_it == surfaces().end()) {
        return std::nullopt;
    }
    static std::mutex mutex;
    static std::map<std::string, GeneratedSet, std::less<>> cache;
    std::lock_guard lock(mutex);
    auto it = cache.find(id);
    if (it == cache.end()) {
        it = cache.emplace(std::string(id), generate(*def_it)).first;
    }
    const core::Image& image = it->second.images[static_cast<std::size_t>(slot_it - kSlotNames.begin())];
    if (image.empty()) {
        return std::nullopt;
    }
    return image;
}

TerrainLayer builtin_layer(std::string_view id) {
    TerrainLayer layer;
    const auto it = std::find_if(builtin_surfaces().begin(), builtin_surfaces().end(),
                                 [&](const BuiltinSurface& s) { return id == s.id; });
    if (it == builtin_surfaces().end()) {
        return layer;
    }
    layer.name = it->name;
    layer.color = glm::vec3(1.0f); // the texture's own colour
    layer.tiling = it->tiling;
    layer.triplanar = it->triplanar;
    for (const LayerTextureSlot slot :
         {LayerTextureSlot::Albedo, LayerTextureSlot::Normal, LayerTextureSlot::Roughness, LayerTextureSlot::Height}) {
        layer_texture(layer, slot) = core::AssetRef{core::Guid{}, builtin_texture_path(it->id, slot)};
    }
    return layer;
}

core::Image water_detail_texture(std::uint32_t size) {
    F3D_ZONE;
    const std::uint32_t n = std::max(size, 8u);
    std::vector<float> heights(static_cast<std::size_t>(n) * n);
    // Directional waves with whole cycles across the tile (so it tiles), plus noise.
    constexpr std::array<std::array<float, 3>, 6> kWaves = {{
        {3.0f, 1.0f, 0.30f}, {-2.0f, 4.0f, 0.22f}, {5.0f, -3.0f, 0.16f},
        {1.0f, 7.0f, 0.10f}, {-8.0f, -5.0f, 0.07f}, {11.0f, 4.0f, 0.05f},
    }};
    core::jobs::parallel_for(
        n,
        [&](std::size_t begin, std::size_t end) {
            for (auto y = static_cast<std::uint32_t>(begin); y < end; ++y) {
                for (std::uint32_t x = 0; x < n; ++x) {
                    const float u = static_cast<float>(x) / static_cast<float>(n);
                    const float v = static_cast<float>(y) / static_cast<float>(n);
                    float h = 0.0f;
                    for (std::size_t w = 0; w < kWaves.size(); ++w) {
                        const auto& [fx, fy, a] = kWaves[w];
                        const float phase = fbm(u, v, 2, 2, 200u + static_cast<std::uint32_t>(w)) * 1.5f;
                        h += a * std::sin((fx * u + fy * v) * kTau + phase);
                    }
                    h = h * 0.55f + fbm(u, v, 6, 5, 210u) * 0.45f; // mostly noise: no visible pattern
                    heights[static_cast<std::size_t>(y) * n + x] = h * 0.5f + 0.5f;
                }
            }
        },
        16);
    core::Image image;
    image.width = image.height = n;
    image.rgba.resize(static_cast<std::size_t>(n) * n * 4);
    const auto at = [&](std::int32_t x, std::int32_t y) {
        return heights[static_cast<std::size_t>(wrap(y, static_cast<std::int32_t>(n))) * n +
                       static_cast<std::size_t>(wrap(x, static_cast<std::int32_t>(n)))];
    };
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const float strength = static_cast<float>(n) / 64.0f;
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const auto ix = static_cast<std::int32_t>(x);
            const auto iy = static_cast<std::int32_t>(y);
            const float dx = (at(ix + 1, iy) - at(ix - 1, iy)) * 0.5f * strength;
            const float dy = (at(ix, iy + 1) - at(ix, iy - 1)) * 0.5f * strength;
            const float u = static_cast<float>(x) / static_cast<float>(n);
            const float v = static_cast<float>(y) / static_cast<float>(n);
            const Cell cell = voronoi(u, v, 12, 220u, 1.0f);
            const float foam = std::clamp(1.0f - (cell.f2 - cell.f1) * 6.0f, 0.0f, 1.0f) * 0.7f +
                               (fbm(u, v, 16, 3, 221u) * 0.5f + 0.5f) * 0.3f;
            std::uint8_t* p = image.rgba.data() + (static_cast<std::size_t>(y) * n + x) * 4;
            p[0] = byte(0.5f - dx * 0.5f);
            p[1] = byte(0.5f - dy * 0.5f);
            p[2] = byte(foam);
            p[3] = byte(at(ix, iy));
        }
    }
    return image;
}

const std::vector<LandscapePreset>& landscape_presets() {
    static const std::vector<LandscapePreset> presets = {
        {"grassland", "Grassland", "Grass, dirt on slopes, rock on cliffs"},
        {"alpine", "Alpine", "Meadows, rock and cliffs, snow on the peaks"},
        {"desert", "Desert", "Sand, gravel on slopes, layered cliffs"},
        {"forest", "Forest", "Forest floor, mud in the hollows, rock on cliffs"},
        {"coast", "Coast", "Grass, sand beaches near the lowest ground, rock on cliffs"},
    };
    return presets;
}

std::vector<TerrainLayer> landscape_preset_layers(std::string_view id, float low, float high) {
    const float range = std::max(high - low, 1.0f);
    const auto slope = [](std::string_view surface, float from, float to, float blend) {
        TerrainLayer layer = builtin_layer(surface);
        layer.rule = {true, -1.0e4f, 1.0e4f, from, to, blend};
        return layer;
    };
    const auto height = [&](std::string_view surface, float from, float to, float max_slope) {
        TerrainLayer layer = builtin_layer(surface);
        layer.rule = {true, low + from * range, to >= 1.0f ? 1.0e4f : low + to * range, 0.0f, max_slope,
                      std::max(range * 0.03f, 1.0f)};
        return layer;
    };
    if (id == "grassland") {
        return {builtin_layer("grass"), slope("dirt", 24.0f, 90.0f, 8.0f), slope("rock", 36.0f, 90.0f, 6.0f)};
    }
    if (id == "alpine") {
        return {builtin_layer("meadow"), slope("rock", 30.0f, 90.0f, 8.0f), slope("cliff", 48.0f, 90.0f, 6.0f),
                height("snow", 0.72f, 1.0f, 42.0f)};
    }
    if (id == "desert") {
        return {builtin_layer("sand"), slope("gravel", 16.0f, 90.0f, 8.0f), slope("cliff", 30.0f, 90.0f, 6.0f)};
    }
    if (id == "forest") {
        return {builtin_layer("forest_floor"), height("mud", -1.0f, 0.08f, 12.0f), slope("dirt", 26.0f, 90.0f, 8.0f),
                slope("rock", 38.0f, 90.0f, 6.0f)};
    }
    if (id == "coast") {
        return {builtin_layer("grass"), height("sand", -1.0f, 0.07f, 25.0f), slope("rock", 34.0f, 90.0f, 6.0f)};
    }
    return {};
}

std::vector<std::uint8_t> resample_tileable(const core::Image& image, std::uint32_t size) {
    F3D_ZONE;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * size * 4, 255);
    if (image.empty() || size == 0) {
        return out;
    }
    std::vector<std::uint8_t> src = image.rgba;
    std::uint32_t w = image.width;
    std::uint32_t h = image.height;
    while (w > size * 2 && h > size * 2 && w >= 2 && h >= 2) {
        const std::uint32_t nw = w / 2;
        const std::uint32_t nh = h / 2;
        std::vector<std::uint8_t> half(static_cast<std::size_t>(nw) * nh * 4);
        for (std::uint32_t y = 0; y < nh; ++y) {
            for (std::uint32_t x = 0; x < nw; ++x) {
                for (std::uint32_t c = 0; c < 4; ++c) {
                    const auto at = [&](std::uint32_t sx, std::uint32_t sy) {
                        return static_cast<std::uint32_t>(src[(static_cast<std::size_t>(sy) * w + sx) * 4 + c]);
                    };
                    const std::uint32_t sum =
                        at(x * 2, y * 2) + at(x * 2 + 1, y * 2) + at(x * 2, y * 2 + 1) + at(x * 2 + 1, y * 2 + 1);
                    half[(static_cast<std::size_t>(y) * nw + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        src = std::move(half);
        w = nw;
        h = nh;
    }
    if (w == size && h == size) {
        return src;
    }
    for (std::uint32_t y = 0; y < size; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(h) / static_cast<float>(size) - 0.5f;
        const float y0f = std::floor(fy);
        const float ty = fy - y0f;
        const auto y0 = static_cast<std::uint32_t>(wrap(static_cast<std::int32_t>(y0f), static_cast<std::int32_t>(h)));
        const auto y1 = static_cast<std::uint32_t>(wrap(static_cast<std::int32_t>(y0f) + 1, static_cast<std::int32_t>(h)));
        for (std::uint32_t x = 0; x < size; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(w) / static_cast<float>(size) - 0.5f;
            const float x0f = std::floor(fx);
            const float tx = fx - x0f;
            const auto x0 =
                static_cast<std::uint32_t>(wrap(static_cast<std::int32_t>(x0f), static_cast<std::int32_t>(w)));
            const auto x1 =
                static_cast<std::uint32_t>(wrap(static_cast<std::int32_t>(x0f) + 1, static_cast<std::int32_t>(w)));
            for (std::uint32_t c = 0; c < 4; ++c) {
                const auto at = [&](std::uint32_t sx, std::uint32_t sy) {
                    return static_cast<float>(src[(static_cast<std::size_t>(sy) * w + sx) * 4 + c]);
                };
                const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                out[(static_cast<std::size_t>(y) * size + x) * 4 + c] =
                    static_cast<std::uint8_t>(std::lround(std::clamp(a + (b - a) * ty, 0.0f, 255.0f)));
            }
        }
    }
    return out;
}

} // namespace f3d::terrain
