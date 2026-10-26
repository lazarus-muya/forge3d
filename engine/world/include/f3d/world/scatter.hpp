// SPDX-License-Identifier: Apache-2.0
#pragma once

// Object scatter: trees, grass, rocks... spread over a terrain (or flat ground) by
// rules. A Scatter component holds the settings; its instances are derived from them
// (deterministic for a seed) and cached per entity: they are regenerated when the
// settings, the entity's position or the terrain's heights or layers change. The
// raster engine draws them, and picking one selects the scatter entity.
//
// phases.md P7.1: rule-based fill with minimum spacing, thinned or filled in by a
// painted density mask (scatter_paint.hpp); instances themselves aren't saved, they are
// regenerated from the settings and the mask.

#include "f3d/assets/mesh.hpp"
#include "f3d/core/asset_ref.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::terrain {
class Heightfield;
struct Terrain;
} // namespace f3d::terrain

namespace f3d::world {

/// Scatter settings: what to place, where, and how many.
struct Scatter {
    core::Guid terrain;                 ///< the surface: a terrain entity; null: flat ground at the entity's height
    std::vector<core::AssetRef> meshes; ///< one is picked at random per instance
    glm::vec2 area{64.0f, 64.0f};       ///< metres (x, z), centred on the entity, aligned to the world axes
    float density = 0.05f;              ///< candidates per square metre, before the rules
    float min_spacing = 1.0f;           ///< metres between any two instances (0: no minimum)
    float scale_min = 0.8f;
    float scale_max = 1.2f;
    bool random_yaw = true;
    float align_to_normal = 0.0f; ///< 0: upright, 1: along the surface normal
    float sink = 0.0f;            ///< metres pushed into the ground (times the instance's scale)
    float height_min = -1.0e4f;   ///< world metres
    float height_max = 1.0e4f;
    float slope_min = 0.0f; ///< degrees
    float slope_max = 90.0f;
    std::int32_t layer = -1;      ///< terrain layer (Terrain.layers index) the ground must be; -1: any
    float layer_threshold = 0.5f; ///< minimum weight of that layer (0-1)
    std::uint32_t seed = 1;
    std::uint32_t max_instances = 100000;
    float draw_distance = 1000.0f; ///< metres from the camera
    bool cast_shadows = true;
    bool visible = true;
    /// Nothing is placed until painted with the density brush (else: everywhere the rules allow, until erased).
    bool paint_only = false;

    friend bool operator==(const Scatter&, const Scatter&) = default;
};

/// Candidates generate_scatter() considers at most (density x area is clamped to it).
inline constexpr std::uint64_t kMaxScatterCandidates = 4'000'000;

struct ScatterInstance {
    glm::mat4 world{1.0f};
    std::uint32_t mesh = 0; ///< index into Scatter::meshes
};

/// What the instances stand on.
struct ScatterSurface {
    const terrain::Heightfield* heights = nullptr; ///< null: flat ground at origin.y
    const terrain::Terrain* terrain = nullptr;     ///< its layers (for Scatter::layer)
    glm::vec3 origin{0.0f};                        ///< the terrain's world origin
    /// Painted density over the scatter's area (null: full, or empty for paint_only).
    const class ScatterMask* mask = nullptr;
    /// World x/z points nothing may stand on (roads, rivers), or empty.
    std::function<bool(float x, float z)> excluded;
};

/// The instances of `scatter` over its area centred at `centre` (world). Deterministic
/// for the same inputs (any thread count); raising the density keeps the earlier
/// candidates. Empty without meshes.
[[nodiscard]] std::vector<ScatterInstance> generate_scatter(const Scatter& scatter, const glm::vec3& centre,
                                                            const ScatterSurface& surface);

/// Registers Scatter with reflection and as a scene component type. Idempotent.
void register_types();

/// The instances of a Scatter entity, regenerated first if stale. Empty for other entities.
[[nodiscard]] const std::vector<ScatterInstance>& scatter_instances(scene::World& world, entt::entity e);

/// Forgets cached instances of entities that are gone or no longer scatter (once per frame).
void prune_scatter_cache(scene::World& world);

// --- Built-in meshes -------------------------------------------------------------------

// Vertex-coloured placeholder meshes (metres, base at the origin), so a scatter shows
// something before any vegetation is imported. Register them with
// assets::MeshLibrary::add under these paths.
inline constexpr const char* kBuiltinPine = "builtin:scatter/pine";
inline constexpr const char* kBuiltinBroadleaf = "builtin:scatter/broadleaf";
inline constexpr const char* kBuiltinGrass = "builtin:scatter/grass";
inline constexpr const char* kBuiltinRock = "builtin:scatter/rock";

/// Every built-in scatter mesh with its path.
[[nodiscard]] std::vector<std::pair<std::string, assets::MeshData>> builtin_scatter_meshes();

/// A ready-made scatter: name and settings (without a terrain).
struct ScatterPreset {
    const char* id;   ///< "trees", "grass", "rocks"
    const char* name; ///< "Trees"
    Scatter scatter;
    bool fit_terrain = true; ///< cover a whole terrain; false: scatter.area around the 3D cursor
};

/// Trees, grass and rocks, using the built-in meshes.
[[nodiscard]] const std::vector<ScatterPreset>& scatter_presets();

} // namespace f3d::world
