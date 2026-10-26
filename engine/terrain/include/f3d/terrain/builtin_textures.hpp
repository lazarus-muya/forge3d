// SPDX-License-Identifier: Apache-2.0
#pragma once

// Built-in terrain texture sets: tileable colour, normal, roughness and height maps
// generated procedurally (no files), so a landscape looks like grass, rock or sand
// before anything is imported. Layers reference them like any image:
// "builtin:terrain/<id>/<slot>" (slot: albedo, normal, roughness, height).

#include "f3d/core/image.hpp"
#include "f3d/terrain/terrain.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::terrain {

/// Prefix of every built-in terrain texture path.
inline constexpr std::string_view kBuiltinTexturePrefix = "builtin:terrain/";

struct BuiltinSurface {
    const char* id;   ///< "grass"
    const char* name; ///< "Grass"
    float tiling;     ///< metres per repeat that looks right
    bool triplanar;   ///< meant for steep ground
};

/// Every built-in surface: grass, meadow, forest floor, dirt, mud, rock, cliff, sand,
/// gravel, snow, asphalt, cobblestone.
[[nodiscard]] const std::vector<BuiltinSurface>& builtin_surfaces();

/// "builtin:terrain/<id>/<slot>".
[[nodiscard]] std::string builtin_texture_path(std::string_view id, LayerTextureSlot slot);

/// The image a built-in path names (generated on first use, then cached; thread-safe),
/// or nullopt for an unknown path. Colour maps are sRGB; the others linear grey (R=G=B)
/// or an OpenGL-style normal map.
[[nodiscard]] std::optional<core::Image> builtin_texture(std::string_view path);

/// A layer using a built-in surface (textures, tiling, triplanar), named after it.
[[nodiscard]] TerrainLayer builtin_layer(std::string_view id);

/// A ready-made set of layers with rules ("auto material").
struct LandscapePreset {
    const char* id;   ///< "grassland"
    const char* name; ///< "Grassland"
    const char* description;
};
/// grassland, alpine, desert, forest, coast.
[[nodiscard]] const std::vector<LandscapePreset>& landscape_presets();
/// The layers of a preset for a terrain whose ground spans [low, high] metres; empty if
/// the id is unknown.
[[nodiscard]] std::vector<TerrainLayer> landscape_preset_layers(std::string_view id, float low, float high);

/// `image` resampled to `size` x `size` RGBA8 as a tiling texture (edges wrap): halved
/// with a box filter while more than twice as large, then bilinear.
[[nodiscard]] std::vector<std::uint8_t> resample_tileable(const core::Image& image, std::uint32_t size);

/// Tiling ripple detail for water surfaces: rg the surface slope (0.5: flat, OpenGL
/// style), b a foam pattern, a the height. Deterministic.
[[nodiscard]] core::Image water_detail_texture(std::uint32_t size = 256);

/// Texels per side of the built-in images.
inline constexpr std::uint32_t kBuiltinTextureSize = 512;

} // namespace f3d::terrain
