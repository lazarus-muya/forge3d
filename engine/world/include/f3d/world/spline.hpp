// SPDX-License-Identifier: Apache-2.0
#pragma once

// Splines (phases.md P7.4): roads, paths, rivers and fences drawn as a few control
// points. A Spline component holds the points (relative to its entity's position) and
// what the spline does; everything it does is derived, never baked:
//
//   - shape the terrain: flatten it to the curve across `width` (minus `depth` in the
//     middle: a riverbed), blending back over `falloff` on each side;
//   - paint a terrain layer under it;
//   - lay a surface along it (a ribbon mesh with a built-in surface texture: asphalt,
//     gravel...);
//   - place meshes along it every `mesh_spacing` metres (fence posts and rails, lamps);
//   - keep scatter (trees, grass, rocks) off it;
//   - rivers: a flowing water surface along the curve.
//
// The terrain effects go into the terrain's derived surface (terrain::HeightfieldStore:
// the authored heights and layer weights with every spline applied), rebuilt for the
// touched tiles whenever a spline or the authored data changes (update_landscape). So
// moving a point re-shapes the ground and deleting the spline restores it exactly;
// sculpting and painting keep working on the authored data underneath.

#include "f3d/assets/mesh.hpp"
#include "f3d/core/asset_ref.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::world {

struct Scatter;

enum class SplineKind : std::uint8_t { Road, Path, River, Fence, Custom };

[[nodiscard]] std::string_view to_string(SplineKind kind) noexcept;
/// "road", "path", "river", "fence", "custom".
bool parse(std::string_view text, SplineKind& kind) noexcept;

struct Spline {
    SplineKind kind = SplineKind::Road;
    core::Guid terrain;             ///< the terrain it shapes and paints (null: none)
    std::vector<glm::vec3> points;  ///< control points, relative to the entity's position
    bool closed = false;            ///< a loop
    float width = 8.0f;             ///< metres
    // Terrain
    bool shape_terrain = true;
    float falloff = 6.0f;           ///< metres of blend beyond each edge
    float depth = 0.0f;             ///< metres carved below the curve at the centre (riverbeds)
    std::int32_t paint_layer = -1;  ///< terrain layer painted under it (-1: none)
    float paint_strength = 1.0f;
    // Surface
    std::string surface = "asphalt"; ///< built-in surface laid along it (terrain::builtin_surfaces); empty: none
    float surface_tiling = 6.0f;     ///< metres per texture repeat
    // Meshes along it
    std::vector<core::AssetRef> meshes; ///< placed in turn every mesh_spacing metres (fences: posts and rails)
    float mesh_spacing = 3.0f;
    float mesh_offset = 0.0f;           ///< sideways (metres, + right); both_sides mirrors it
    bool both_sides = false;
    // Scatter
    bool clear_scatter = true;
    float clear_margin = 1.0f; ///< metres beyond the edges kept clear
    // Rivers
    float flow_speed = 1.0f; ///< metres per second along the curve
    bool visible = true;

    friend bool operator==(const Spline&, const Spline&) = default;
};

/// Sensible settings for a kind (width, shaping, surface, meshes...), without points.
[[nodiscard]] Spline spline_preset(SplineKind kind);

/// Registers Spline (and SplineKind) with reflection and as a scene component. Idempotent.
void register_spline_types();

// --- Curves ------------------------------------------------------------------------------

struct SplineSample {
    glm::vec3 position{0.0f};
    glm::vec3 tangent{0.0f, 0.0f, 1.0f}; ///< unit
    float distance = 0.0f;               ///< along the curve from its start
};

/// A centripetal Catmull-Rom curve through `points` (world), sampled about every
/// `step` metres (each control point is a sample). One point: that point.
[[nodiscard]] std::vector<SplineSample> evaluate_spline(const std::vector<glm::vec3>& points, bool closed,
                                                        float step = 1.0f);

/// The world positions of a Spline entity's control points.
[[nodiscard]] std::vector<glm::vec3> spline_world_points(scene::World& world, entt::entity spline);

/// The horizontal distance from (x, z) to the curve and the curve's height and distance
/// along there. A polyline of samples, bucketed for fast queries.
class SplineShape {
public:
    SplineShape(const std::vector<SplineSample>& samples, float reach);

    struct Hit {
        float distance = 0.0f; ///< horizontal, metres
        float height = 0.0f;   ///< of the curve at the nearest point
        float along = 0.0f;    ///< curve distance of the nearest point
    };
    /// The nearest point of the curve within `reach` of (x, z), or nullopt.
    [[nodiscard]] std::optional<Hit> nearest(float x, float z) const;
    /// x/z bounds of everything within reach: min, max.
    [[nodiscard]] const glm::vec4& bounds() const noexcept { return bounds_; }
    [[nodiscard]] bool empty() const noexcept { return segments_.empty(); }

private:
    struct Segment {
        glm::vec3 a, b;
        float along_a, along_b;
    };
    std::vector<Segment> segments_;
    float reach_;
    float cell_;
    glm::vec4 bounds_{0.0f};
    glm::ivec2 origin_{0};
    glm::ivec2 size_{0};
    std::vector<std::vector<std::uint32_t>> cells_;
};

// --- The landscape ------------------------------------------------------------------------

/// Brings every terrain's derived surface up to date with its splines and its
/// authored data (only touched tiles are rebuilt). Call once per frame before drawing,
/// raycasting or scattering against terrains; cheap when nothing changed.
void update_landscape(scene::World& world);

/// A value that changes whenever any spline changes (or one is added or removed).
[[nodiscard]] std::uint64_t splines_revision(scene::World& world);

/// Where a scatter may not place instances (on splines that clear scatter), and a
/// revision that changes whenever that does. Empty function: nowhere.
[[nodiscard]] std::function<bool(float x, float z)> scatter_exclusion(scene::World& world, const Scatter& scatter,
                                                                      std::uint64_t& revision);

// --- Geometry -----------------------------------------------------------------------------

/// A mesh placed along a spline.
struct SplineProp {
    core::AssetRef mesh;
    glm::mat4 world{1.0f};
};

/// A strip of flowing water (rivers): world positions, flow (x/z metres per second)
/// per vertex, triangles.
struct WaterStrip {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> flow;
    std::vector<std::uint32_t> indices;
};

/// What a spline draws: its surface ribbon (world space, UVs in metres / tiling), the
/// meshes along it, and a river's water.
struct SplineGeometry {
    std::shared_ptr<const assets::MeshData> surface;
    std::vector<SplineProp> props;
    std::shared_ptr<const WaterStrip> water;
    std::uint64_t revision = 0; ///< changes whenever any of them does
};

/// The geometry of a Spline entity, rebuilt if stale (call update_landscape first).
[[nodiscard]] const SplineGeometry& spline_geometry(scene::World& world, entt::entity spline);

// Built-in meshes for fences (metres): a post (0.12 x 1.2 x 0.12, base at the origin) and
// a rail one metre long along +Z (centred), scaled to each gap.
inline constexpr const char* kBuiltinFencePost = "builtin:spline/fence_post";
inline constexpr const char* kBuiltinFenceRail = "builtin:spline/fence_rail";
[[nodiscard]] std::vector<std::pair<std::string, assets::MeshData>> builtin_spline_meshes();

} // namespace f3d::world
