// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/render_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace f3d::render::raster {
namespace {

constexpr std::array<std::pair<ViewMode, std::string_view>, 5> kModes = {{
    {ViewMode::Lit, "lit"},
    {ViewMode::Unlit, "unlit"},
    {ViewMode::Wireframe, "wireframe"},
    {ViewMode::Normals, "normals"},
    {ViewMode::Cascades, "cascades"},
}};

constexpr std::array<std::pair<Tonemapper, std::string_view>, 3> kTonemappers = {{
    {Tonemapper::AgX, "agx"},
    {Tonemapper::Aces, "aces"},
    {Tonemapper::None, "none"},
}};

constexpr std::array<std::pair<FogQuality, std::string_view>, 5> kFogQualities = {{
    {FogQuality::Off, "off"},
    {FogQuality::Low, "low"},
    {FogQuality::Medium, "medium"},
    {FogQuality::High, "high"},
    {FogQuality::Epic, "epic"},
}};

constexpr std::array<std::pair<Scalability, std::string_view>, 4> kScalability = {{
    {Scalability::Low, "low"},
    {Scalability::Medium, "medium"},
    {Scalability::High, "high"},
    {Scalability::Epic, "epic"},
}};

template <class T, std::size_t N>
std::string_view name_of(const std::array<std::pair<T, std::string_view>, N>& table, T value) noexcept {
    for (const auto& [v, name] : table) {
        if (v == value) {
            return name;
        }
    }
    return table[0].second;
}

template <class T, std::size_t N>
std::optional<T> parse_name(const std::array<std::pair<T, std::string_view>, N>& table, std::string_view text) noexcept {
    for (const auto& [v, name] : table) {
        if (name == text) {
            return v;
        }
    }
    return std::nullopt;
}

/// CIE 1931 xy of a blackbody (Kim et al. 2002 cubic fit, 1667-25000 K).
glm::vec2 planckian_xy(float kelvin) {
    const double t = std::clamp(static_cast<double>(kelvin), 1667.0, 25000.0);
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                                 : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x;
    const double x3 = x2 * x;
    double y = 0.0;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
    return {static_cast<float>(x), static_cast<float>(y)};
}

/// XYZ (Y = 1) of a white point at `kelvin`, moved off the Planckian locus by `duv`
/// (CIE 1960 uv, + towards green; the locus normal is taken as +v).
glm::vec3 white_xyz(float kelvin, float duv) {
    const glm::vec2 xy = planckian_xy(kelvin);
    const float d = -2.0f * xy.x + 12.0f * xy.y + 3.0f;
    glm::vec2 uv(4.0f * xy.x / d, 6.0f * xy.y / d);
    uv.y += duv;
    const float e = 2.0f * uv.x - 8.0f * uv.y + 4.0f;
    const glm::vec2 adjusted(3.0f * uv.x / e, 2.0f * uv.y / e);
    return {adjusted.x / adjusted.y, 1.0f, (1.0f - adjusted.x - adjusted.y) / adjusted.y};
}

glm::vec3 pow3(const glm::vec3& v, float e) {
    return {std::pow(v.x, e), std::pow(v.y, e), std::pow(v.z, e)};
}

// AgX (Troy Sobotka), as in Benjamin Wrensch's minimal fit. Matrices are column-major
// (GLSL order), so M * v here equals mul(v, M) in shaders/resolve.ps.hlsl.
glm::vec3 agx(glm::vec3 v) {
    const glm::mat3 inset(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f, 0.0784335999999992f,
                          0.878468636469772f, 0.0784336f, 0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
    const glm::mat3 outset(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f, -0.0980208811401368f,
                           1.15190312990417f, -0.0980434501171241f, -0.0990297440797205f, -0.0989611768448433f,
                           1.15107367264116f);
    constexpr float kMinEv = -12.47393f;
    constexpr float kMaxEv = 4.026069f;
    v = inset * glm::max(v, glm::vec3(1e-10f));
    v = glm::clamp(glm::vec3(std::log2(v.x), std::log2(v.y), std::log2(v.z)), kMinEv, kMaxEv);
    v = (v - kMinEv) / (kMaxEv - kMinEv);
    const glm::vec3 v2 = v * v;
    const glm::vec3 v4 = v2 * v2;
    v = 15.5f * v4 * v2 - 40.14f * v4 * v + 31.96f * v4 - 6.868f * v2 * v + 0.4298f * v2 + 0.1191f * v - 0.00232f;
    v = outset * v;
    return glm::clamp(pow3(glm::max(v, glm::vec3(0.0f)), 2.2f), 0.0f, 1.0f);
}

// ACES RRT + ODT fit (Stephen Hill); matrices row-major as in his HLSL.
glm::vec3 aces(const glm::vec3& c) {
    const glm::mat3 input = glm::transpose(
        glm::mat3(0.59719f, 0.35458f, 0.04823f, 0.07600f, 0.90834f, 0.01566f, 0.02840f, 0.13383f, 0.83777f));
    const glm::mat3 output = glm::transpose(
        glm::mat3(1.60475f, -0.53108f, -0.07367f, -0.10208f, 1.10813f, -0.00605f, -0.00327f, -0.07276f, 1.07602f));
    const glm::vec3 v = input * c;
    const glm::vec3 a = v * (v + 0.0245786f) - 0.000090537f;
    const glm::vec3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return glm::clamp(output * (a / b), 0.0f, 1.0f);
}

} // namespace

std::string_view to_string(ViewMode mode) noexcept {
    for (const auto& [value, name] : kModes) {
        if (value == mode) {
            return name;
        }
    }
    return "lit";
}

std::optional<ViewMode> parse_view_mode(std::string_view text) noexcept {
    for (const auto& [value, name] : kModes) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::string_view to_string(Tonemapper tonemapper) noexcept {
    for (const auto& [value, name] : kTonemappers) {
        if (value == tonemapper) {
            return name;
        }
    }
    return "agx";
}

std::optional<Tonemapper> parse_tonemapper(std::string_view text) noexcept {
    for (const auto& [value, name] : kTonemappers) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

glm::vec3 tonemap(Tonemapper tonemapper, const glm::vec3& color) noexcept {
    switch (tonemapper) {
    case Tonemapper::AgX:
        return agx(color);
    case Tonemapper::Aces:
        return aces(color);
    case Tonemapper::None:
        break;
    }
    return glm::clamp(color, 0.0f, 1.0f);
}

glm::uvec2 scaled_render_size(std::uint32_t width, std::uint32_t height, float percentage) noexcept {
    const float scale = std::clamp(percentage, kMinScreenPercentage, kMaxScreenPercentage) / 100.0f;
    return {std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(width) * scale))),
            std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(height) * scale)))};
}

void apply_scalability(Scalability level, RenderSettings& settings, std::uint32_t& shadow_resolution) noexcept {
    switch (level) {
    case Scalability::Low:
        settings.screen_percentage = 50.0f;
        settings.fog_quality = FogQuality::Low;
        shadow_resolution = 1024;
        break;
    case Scalability::Medium:
        settings.screen_percentage = 75.0f;
        settings.fog_quality = FogQuality::Medium;
        shadow_resolution = 1536;
        break;
    case Scalability::High:
        settings.screen_percentage = 100.0f;
        settings.fog_quality = FogQuality::High;
        shadow_resolution = 2048;
        break;
    case Scalability::Epic:
        settings.screen_percentage = 100.0f;
        settings.fog_quality = FogQuality::Epic;
        shadow_resolution = 4096;
        break;
    }
}

std::string_view to_string(FogQuality quality) noexcept {
    return name_of(kFogQualities, quality);
}

std::optional<FogQuality> parse_fog_quality(std::string_view text) noexcept {
    return parse_name(kFogQualities, text);
}

std::string_view to_string(Scalability level) noexcept {
    return name_of(kScalability, level);
}

std::optional<Scalability> parse_scalability(std::string_view text) noexcept {
    return parse_name(kScalability, text);
}

glm::mat3 white_balance(float temperature, float tint) noexcept {
    // Bradford cone response and linear sRGB -> XYZ (D65), written row by row.
    const glm::mat3 bradford = glm::transpose(
        glm::mat3(0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f, -0.0685f, 1.0296f));
    const glm::mat3 xyz_from_rgb = glm::transpose(glm::mat3(0.4124564f, 0.3575761f, 0.1804375f, 0.2126729f, 0.7151522f,
                                                            0.0721750f, 0.0193339f, 0.1191920f, 0.9503041f));
    // A greener image means the light balanced away is magenta: the source white moves the other way.
    const glm::vec3 source = bradford * white_xyz(temperature, -tint * 0.02f);
    const glm::vec3 target = bradford * white_xyz(6500.0f, 0.0f);
    const glm::mat3 scale(glm::vec3(target.x / source.x, 0.0f, 0.0f), glm::vec3(0.0f, target.y / source.y, 0.0f),
                          glm::vec3(0.0f, 0.0f, target.z / source.z));
    return glm::inverse(xyz_from_rgb) * glm::inverse(bradford) * scale * bradford * xyz_from_rgb;
}

float ev100_from_luminance(float luminance) noexcept {
    return std::log2(std::max(luminance, 1e-6f) * 100.0f / 12.5f);
}

} // namespace f3d::render::raster
