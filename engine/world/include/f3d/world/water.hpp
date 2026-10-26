// SPDX-License-Identifier: Apache-2.0
#pragma once

// Water (phases.md P7.3): lakes, oceans and (with splines) rivers.
//
// A WaterBody component on an entity: the entity's height is the water level.
//   Lake   floods the basin of its terrain around the entity: every connected bit of
//          ground below the level, within `extent`, starting under the entity. Moving
//          the entity up fills the basin further; the surface is derived (lake_surface)
//          and follows the terrain as it is sculpted.
//   Ocean  an endless plane at the level (drawn around the camera).
// Rivers are River splines (spline.hpp): a water strip along the curve, with flow.
//
// The look (shallow and deep colours, clarity, waves, foam, refraction) is shared by
// all three; the renderer draws them after the opaque scene with refraction of what
// is below, absorption by depth, reflections and shore foam.

#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"
#include "f3d/world/spline.hpp"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace f3d::scene {
class World;
}
namespace f3d::terrain {
class Heightfield;
}

namespace f3d::world {

enum class WaterKind : std::uint8_t { Lake, Ocean };

[[nodiscard]] std::string_view to_string(WaterKind kind) noexcept;
/// "lake", "ocean".
bool parse(std::string_view text, WaterKind& kind) noexcept;

struct WaterBody {
    WaterKind kind = WaterKind::Lake;
    core::Guid terrain;             ///< lakes: the terrain whose basin fills (null: the first under the entity)
    glm::vec2 extent{1024.0f, 1024.0f}; ///< lakes: the largest area searched (x, z metres, centred on the entity)
    glm::vec3 shallow_color{0.10f, 0.36f, 0.34f}; ///< linear: the water seen through a thin layer
    glm::vec3 deep_color{0.010f, 0.040f, 0.055f}; ///< linear: the colour of deep water
    float clarity = 5.0f;        ///< metres of water before the bottom fades into the deep colour
    float wave_height = 0.12f;   ///< metres (crest to trough, roughly)
    float wave_length = 5.0f;    ///< metres between crests
    float wind_direction = 30.0f; ///< degrees, around +Y from +X
    float wave_speed = 1.0f;
    float foam = 0.6f;       ///< shore and crest foam (0: none)
    float refraction = 0.5f; ///< how much the bottom wobbles through the waves
    float roughness = 0.04f; ///< of the surface (0: a mirror)
    bool visible = true;

    friend bool operator==(const WaterBody&, const WaterBody&) = default;
};

/// Sensible settings for a kind (oceans have bigger waves, bluer and clearer water).
[[nodiscard]] WaterBody water_preset(WaterKind kind);

/// Registers WaterBody with reflection and as a scene component. Idempotent.
void register_water_types();

/// Flooded cells of a heightfield: a grid of `step`-metre cells whose first cell's
/// corner is at `origin` (terrain-local x, z).
struct FloodResult {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float step = 1.0f;
    glm::vec2 origin{0.0f};
    std::vector<std::uint8_t> wet; ///< 1: under water, row-major
    std::size_t cells = 0;         ///< wet cells
};

/// Fills the basin under `seed` (terrain-local x, z) up to `level` (terrain-local
/// height): the 4-connected cells whose ground is below the level, within the
/// `extent` rectangle centred on the seed. Nothing if the ground at the seed is above
/// the level. At most `max_cells` cells per side (the step grows to fit).
[[nodiscard]] FloodResult flood_basin(const terrain::Heightfield& heights, const glm::vec2& seed, float level,
                                      const glm::vec2& extent, std::uint32_t max_cells = 512);

/// The surface of a lake: the flooded cells plus a one-cell skirt under the shore, at
/// the level (world space). Rebuilt when the lake, its position or its terrain changes.
/// Null for oceans, hidden lakes and lakes with nothing to flood.
[[nodiscard]] std::shared_ptr<const WaterStrip> lake_surface(scene::World& world, entt::entity water);

/// The water level (world height) of a water entity.
[[nodiscard]] float water_level(scene::World& world, entt::entity water);

} // namespace f3d::world
