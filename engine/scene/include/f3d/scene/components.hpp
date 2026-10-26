// SPDX-License-Identifier: Apache-2.0
#pragma once

// Core scene components (design §3.C). Every entity has Name, core::Guid,
// Transform and Hierarchy; the rest are optional.

#include "f3d/core/asset_ref.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f3d::scene {

struct Name {
    std::string value;
};

/// Local translation / rotation / scale relative to the parent, plus the cached
/// world matrix. Write it through World (set_local / mark_dirty) so the cache
/// and the children's caches are refreshed by World::update_transforms().
struct Transform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< w, x, y, z
    glm::vec3 scale{1.0f};

    glm::mat4 world{1.0f}; ///< cached; valid after World::update_transforms()
    bool dirty = true;     ///< local TRS changed since the cache was computed

    [[nodiscard]] glm::mat4 local_matrix() const noexcept;
};

/// Intrusive, ordered tree links. The roots form a sibling chain owned by the World.
/// Maintained by World; never edit it directly.
struct Hierarchy {
    entt::entity parent = entt::null;
    entt::entity first_child = entt::null;
    entt::entity last_child = entt::null;
    entt::entity prev_sibling = entt::null;
    entt::entity next_sibling = entt::null;
    std::uint32_t child_count = 0;
};

/// Draws a mesh asset with the entity's world transform.
struct MeshRenderer {
    core::AssetRef mesh;
    bool visible = true;
    /// Base-colour texture override (a project texture asset): replaces the base-colour
    /// texture of every slot's material; empty: the materials' own.
    core::AssetRef texture;
    /// Material per submesh slot. An empty entry (or a missing one) uses the material
    /// the model assigns to that submesh.
    std::vector<core::AssetRef> materials;
};

enum class LightType : std::uint8_t { Directional, Point, Spot };

/// A light source. Points down the entity's local -Z axis (directional and spot).
struct Light {
    LightType type = LightType::Point;
    glm::vec3 color{1.0f};   ///< linear RGB
    float intensity = 1.0f;  ///< lux (directional) or candela (point/spot)
    float range = 10.0f;     ///< metres (point/spot)
    float inner_cone = 0.4f; ///< radians (spot)
    float outer_cone = 0.6f; ///< radians (spot)
    bool cast_shadows = true;
    /// How strongly the light scatters in volumetric fog (1: physically, 0: not at all).
    float volumetric_scattering = 1.0f;
};

/// Exponential height fog (Unreal's "Exponential Height Fog"): a participating medium
/// whose density falls off exponentially above the entity's height. The first visible
/// entity with one applies. With `volumetric`, the fog near the camera is ray-marched in
/// a froxel volume lit by the sun (with its shadows), local lights and the sky, so light
/// shafts and light cones appear; beyond `volumetric_distance` (and without it) the fog
/// is integrated analytically, lit by the sun and the sky without shadows.
struct HeightFog {
    /// Extinction (1/m) at the entity's height. Visibility is about 3 / density:
    /// 0.0005 clear haze, 0.002 haze (1.5 km), 0.01 mist (300 m), 0.05 thick fog (60 m).
    float density = 0.002f;
    float height_falloff = 0.02f;  ///< 1/m: density halves every ln(2) / falloff metres upwards (0.02: 35 m)
    glm::vec3 albedo{0.9f};        ///< scattering / extinction (linear RGB): grey haze below 1, white mist at 1
    float anisotropy = 0.6f;       ///< Henyey-Greenstein g: 0 isotropic, towards 1 forward scattering (glow around the sun)
    float sky_lighting = 1.0f;     ///< scale of the sky (ambient) light the fog scatters
    float sun_lighting = 1.0f;     ///< scale of the directional light the fog scatters
    float start_distance = 0.0f;   ///< metres from the camera before any fog
    float max_opacity = 1.0f;      ///< 0-1: cap on how much the fog hides
    float cutoff_distance = 0.0f;  ///< metres at which the sky is fogged (0: 30 km)
    bool volumetric = true;        ///< froxel volumetric fog near the camera
    float volumetric_distance = 150.0f; ///< metres the froxel volume covers
    float volumetric_density = 1.0f;    ///< extinction scale inside the volume
    bool visible = true;

    friend bool operator==(const HeightFog&, const HeightFog&) = default;
};

/// A local fog volume (Unreal's "Local Fog Volume"): a box of fog around the entity,
/// `size` metres (times the entity's scale), in its rotation. Thickens the volumetric
/// fog (it needs volumetric fog: a HeightFog with `volumetric`, or none at all, then
/// the volume uses the default froxel settings).
struct FogVolume {
    glm::vec3 size{20.0f, 6.0f, 20.0f}; ///< metres
    float density = 0.1f;               ///< extinction (1/m) at the bottom
    glm::vec3 albedo{0.9f};
    float height_falloff = 0.3f;        ///< 1/m upwards from the bottom of the box (0: uniform)
    float edge_fade = 0.25f;            ///< 0-1: fraction of the box over which the density fades at its sides
    glm::vec3 emissive{0.0f};           ///< radiance emitted per metre (cd/m^2/m): glowing smoke
    bool visible = true;

    friend bool operator==(const FogVolume&, const FogVolume&) = default;
};

/// Post-processing (the camera "film"): bloom, vignette and colour grading. The first
/// visible entity with one applies; without one there is no bloom or grading.
struct PostProcess {
    float bloom_intensity = 0.04f;  ///< 0-1: fraction of the image replaced by its blurred glow (energy conserving)
    float bloom_radius = 0.8f;      ///< 0-1: how far the glow spreads
    float bloom_threshold = 0.0f;   ///< EV above the average exposure before a pixel blooms (0: everything, physically)
    float vignette = 0.3f;          ///< 0-1: natural lens fall-off towards the corners
    float temperature = 6500.0f;    ///< K: white balance (lower: the image gets warmer)
    float tint = 0.0f;              ///< -1 (magenta) to 1 (green)
    float saturation = 1.0f;
    float contrast = 1.0f;
    float film_grain = 0.0f;        ///< 0-1
    bool visible = true;

    friend bool operator==(const PostProcess&, const PostProcess&) = default;
};

/// Physical sky, sun and moon (phases.md P4.3). The first visible entity with a Sky
/// drives the atmosphere, the sun and moon lights, image-based lighting and the
/// automatic exposure; without one the renderer uses a default sun and a flat
/// background. The entity's transform is ignored. World north is -Z, east +X.
struct Sky {
    float time_of_day = 12.0f; ///< local time, hours (0-24)
    std::int32_t year = 2026;
    std::int32_t month = 6;            ///< 1-12
    std::int32_t day = 21;             ///< 1-31
    float latitude = 45.0f;            ///< degrees, + north
    float longitude = 0.0f;            ///< degrees, + east
    float utc_offset = 0.0f;           ///< hours: the time zone of time_of_day
    float north_offset = 0.0f;         ///< degrees: north rotated clockwise (seen from above) from -Z
    float sun_illuminance = 128000.0f; ///< lux, at the top of the atmosphere
    float haze = 1.0f;                 ///< aerosol (Mie) density multiplier
    glm::vec3 ground_albedo{0.3f};
    bool moon = true;                   ///< moonlight when the sun is down
    bool shadows = true;                ///< the sun (or moon) casts shadows
    float exposure_compensation = 0.0f; ///< EV added to the automatic exposure

    friend bool operator==(const Sky&, const Sky&) = default;
};

/// A reflection probe (phases.md P4.3): captures the scene around the entity into a
/// cube map. Meshes whose bounds centre lies within `radius` use the nearest probe
/// for image-based lighting instead of the sky. Re-captured when it moves or changes,
/// when the sky changes, or on request (render.bake_probes).
struct ReflectionProbe {
    float radius = 10.0f;           ///< metres
    std::uint32_t resolution = 128; ///< cube face size (pixels)
};

/// Puts the entity on a layer of the World's layer table (World::layers()). A null
/// layer (or no component) is the default layer, which is always visible and
/// unlocked. An entity is hidden/locked if it or any ancestor is on a hidden/
/// locked layer (scene::is_visible / scene::is_pickable).
struct Layer {
    core::Guid layer;
};

/// A property of a prefab instance that differs from its prefab, kept when the
/// prefab changes (phases.md P3.4).
struct PrefabOverride {
    core::Guid node;   ///< the prefab node (PrefabLink::node) whose property it is
    std::string path;  ///< "Component.field"
    std::string value; ///< the field's JSON, as text
};

/// The root of a prefab instance: which prefab, and the instance's overrides.
struct PrefabInstance {
    core::AssetRef prefab; ///< path: the .f3dprefab file (VFS path)
    std::vector<PrefabOverride> overrides;
};

/// Ties an entity of a prefab instance to its prefab node.
struct PrefabLink {
    core::Guid node;       ///< node id in the prefab of the nearest PrefabInstance (self or ancestor)
    core::Guid outer_node; ///< for the root of a nested instance: its node id in the outer prefab
};

enum class CameraProjection : std::uint8_t { Perspective, Orthographic };

/// A scene camera. Looks down the entity's local -Z axis.
struct Camera {
    CameraProjection projection = CameraProjection::Perspective;
    float fov_y = 0.785398f;    ///< radians (perspective)
    float ortho_height = 10.0f; ///< metres (orthographic)
    float near_plane = 0.05f;
    float far_plane = 5000.0f;
    float aspect_ratio = 1.777778f; ///< width / height of the camera's frame (its preview and frustum)
};

} // namespace f3d::scene
