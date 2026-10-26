// SPDX-License-Identifier: Apache-2.0
#pragma once

// Viewport rendering settings (phases.md P4.4): view mode, tonemapper, exposure,
// screen percentage, fog quality and post-processing; scalability presets set several
// at once. The tonemapping curves have a CPU reference here (the shaders implement the same).

#include "f3d/core/math.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace f3d::render::raster {

enum class ViewMode : std::uint8_t {
    Lit,       ///< full shading, exposure, tonemapping
    Unlit,     ///< base colour + emissive, no lighting
    Wireframe, ///< triangle edges
    Normals,   ///< world-space shading normals as colour
    Cascades,  ///< lit, tinted by shadow cascade
};

enum class Tonemapper : std::uint8_t {
    AgX,  ///< Sobotka's AgX (default): gentle highlight roll-off and desaturation
    Aces, ///< the ACES RRT+ODT fit (Hill)
    None, ///< clamp only
};

enum class ExposureMode : std::uint8_t {
    Auto,   ///< luminance histogram with adaptation (needs a Sky; without one exposure is 1)
    Manual, ///< a fixed EV100
};

struct ExposureSettings {
    ExposureMode mode = ExposureMode::Auto;
    float manual_ev100 = 12.0f;
    float compensation = 0.0f; ///< EV, added to the Sky's own compensation
    float min_ev100 = -2.0f;   ///< automatic exposure limits
    float max_ev100 = 18.0f;
    float speed_up = 3.0f;      ///< adaptation rate (1/s) when the scene gets brighter
    float speed_down = 1.5f;    ///< and darker
    float low_percent = 0.10f;  ///< the darkest pixels ignored by the meter
    float high_percent = 0.90f; ///< pixels above this fraction (the brightest) ignored
};

/// Volumetric fog resolution (froxel tile size and depth slices).
enum class FogQuality : std::uint8_t {
    Off,    ///< analytic height fog only: no light shafts, no fog volumes
    Low,    ///< 16-pixel tiles, 48 slices
    Medium, ///< 12-pixel tiles, 64 slices
    High,   ///< 8-pixel tiles, 64 slices (Unreal's default)
    Epic,   ///< 6-pixel tiles, 96 slices
};

/// Overall quality levels (Unreal's "Engine Scalability"): set the screen percentage,
/// shadow resolution and fog quality together (apply_scalability()).
enum class Scalability : std::uint8_t { Low, Medium, High, Epic };

inline constexpr float kMinScreenPercentage = 25.0f;
inline constexpr float kMaxScreenPercentage = 200.0f;

struct RenderSettings {
    ViewMode mode = ViewMode::Lit;
    Tonemapper tonemapper = Tonemapper::AgX;
    ExposureSettings exposure;
    /// The scene is rendered at this percentage of the output size (25-200) and scaled
    /// to it: below 100 renders faster, above 100 supersamples.
    float screen_percentage = 100.0f;
    FogQuality fog_quality = FogQuality::High;
    /// The scene's PostProcess (bloom, grading) applies; off: plain tonemapping.
    bool post_process = true;
};

/// The render size for an output size at a screen percentage (at least 1x1).
[[nodiscard]] glm::uvec2 scaled_render_size(std::uint32_t width, std::uint32_t height, float percentage) noexcept;

/// Sets `settings`' screen percentage and fog quality, and `shadow_resolution`, for a level.
void apply_scalability(Scalability level, RenderSettings& settings, std::uint32_t& shadow_resolution) noexcept;

[[nodiscard]] std::string_view to_string(FogQuality quality) noexcept;
[[nodiscard]] std::optional<FogQuality> parse_fog_quality(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(Scalability level) noexcept;
[[nodiscard]] std::optional<Scalability> parse_scalability(std::string_view text) noexcept;

/// White balance as a linear-sRGB matrix (camera white balance): light of colour
/// temperature `temperature` (K), shifted towards green (`tint` > 0) or magenta (< 0),
/// comes out white, as a 6500 K light does by default (von Kries adaptation in Bradford
/// cone space). Lower temperatures make daylight look bluer. Identity at 6500 K, tint 0.
[[nodiscard]] glm::mat3 white_balance(float temperature, float tint) noexcept;

[[nodiscard]] std::string_view to_string(ViewMode mode) noexcept;
[[nodiscard]] std::optional<ViewMode> parse_view_mode(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(Tonemapper tonemapper) noexcept;
[[nodiscard]] std::optional<Tonemapper> parse_tonemapper(std::string_view text) noexcept;

/// Scene-linear (pre-exposed) colour -> display-linear colour in [0, 1].
[[nodiscard]] glm::vec3 tonemap(Tonemapper tonemapper, const glm::vec3& color) noexcept;

/// EV100 a reflected-light meter reads for an average luminance (cd/m2): log2(L * 100 / 12.5).
[[nodiscard]] float ev100_from_luminance(float luminance) noexcept;

} // namespace f3d::render::raster
