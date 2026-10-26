// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scene lights for the clustered forward+ pass (phases.md P4.2).
//
// Local lights (point, spot) are binned into a froxel grid: kTilesX x kTilesY
// screen tiles x kSlices depth slices, exponentially spaced between the near and
// far planes. A compute pass (shaders/light_cluster.cs.hlsl) writes each
// cluster's light list; the forward pass shades a pixel with the lights of its
// cluster only. Directional lights are few and apply everywhere (constants).
//
// The functions here are the CPU reference of the cluster math; the shaders
// implement the same formulas (tests compare them).

#include "f3d/core/math.hpp"
#include "f3d/render/view.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::render::raster {

inline constexpr std::uint32_t kClusterTilesX = 16;
inline constexpr std::uint32_t kClusterTilesY = 9;
inline constexpr std::uint32_t kClusterSlices = 24;
inline constexpr std::uint32_t kClusterCount = kClusterTilesX * kClusterTilesY * kClusterSlices;
inline constexpr std::uint32_t kMaxLightsPerCluster = 64;
inline constexpr std::uint32_t kMaxDirectionalLights = 4;
inline constexpr std::uint32_t kMaxLocalLights = 4096;

/// GPU layout of shaders/lighting.hlsli `GpuLight`.
struct GpuLight {
    glm::vec3 position{0.0f};               ///< world space
    float range = 10.0f;                    ///< metres; no light beyond it
    glm::vec3 color{1.0f};                  ///< colour x intensity (candela)
    std::uint32_t type = 0;                 ///< 0 point, 1 spot
    glm::vec3 direction{0.0f, 0.0f, -1.0f}; ///< spot axis (world, unit, away from the light)
    float spot_scale = 0.0f;                ///< angular falloff: saturate(cos(angle) * scale + offset)^2
    float spot_offset = 1.0f;
    float volumetric = 1.0f; ///< scattering scale in volumetric fog (Light::volumetric_scattering)
    float pad[2] = {0.0f, 0.0f};
};
static_assert(sizeof(GpuLight) == 64);

struct DirectionalLight {
    glm::vec3 direction{0.0f, 1.0f, 0.0f}; ///< unit, towards the light
    glm::vec3 color{1.0f};                 ///< colour x illuminance (lux)
    bool cast_shadows = false;
    float volumetric = 1.0f; ///< scattering scale in volumetric fog
};

struct SceneLights {
    std::vector<GpuLight> local;
    std::vector<DirectionalLight> directional; ///< at most kMaxDirectionalLights
};

/// The visible Light components of `world` (transforms updated first). Directional
/// lights beyond kMaxDirectionalLights and local lights beyond kMaxLocalLights are dropped.
[[nodiscard]] SceneLights gather_lights(scene::World& world);

/// Depth slicing of a view: slice = floor(log(depth) * scale + bias).
struct ClusterDepth {
    float near_plane = 0.05f;
    float far_plane = 5000.0f;
    float scale = 1.0f;
    float bias = 0.0f;
};
[[nodiscard]] ClusterDepth cluster_depth(const View& view) noexcept;
/// The depth slice of a view-space depth (distance in front of the camera).
[[nodiscard]] std::uint32_t cluster_slice(float view_depth, const ClusterDepth& depth) noexcept;
/// Tile size in pixels (the grid covers the view; edge tiles may stick out).
[[nodiscard]] glm::uvec2 cluster_tile_size(const View& view) noexcept;
/// Flat cluster index (x fastest, then y, then slice).
[[nodiscard]] constexpr std::uint32_t cluster_index(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (z * kClusterTilesY + y) * kClusterTilesX + x;
}
/// The cluster containing pixel (px, py) at a view-space depth.
[[nodiscard]] std::uint32_t cluster_at(const View& view, float px, float py, float view_depth) noexcept;

/// View-space bounding box of a cluster.
struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};
[[nodiscard]] Aabb cluster_bounds(const View& view, std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept;
[[nodiscard]] bool sphere_intersects(const Aabb& box, const glm::vec3& centre, float radius) noexcept;

/// CPU reference of the light-clustering pass: for every cluster, the indices of
/// the local lights touching it (in light order, at most kMaxLightsPerCluster).
[[nodiscard]] std::vector<std::vector<std::uint32_t>> assign_lights(const View& view, std::span<const GpuLight> lights);

} // namespace f3d::render::raster
