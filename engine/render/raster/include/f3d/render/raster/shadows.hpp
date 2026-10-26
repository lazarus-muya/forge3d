// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cascaded shadow maps (phases.md P4.3): the view's shadow distance is split into
// four cascades (practical split scheme). Each cascade is fitted with a bounding
// sphere of its frustum slice, so its size doesn't change as the camera turns, and
// its light-space centre is snapped to whole shadow-map texels, so moving the
// camera doesn't make shadow edges crawl.

#include "f3d/core/math.hpp"
#include "f3d/render/view.hpp"

#include <array>
#include <cstdint>

namespace f3d::render::raster {

inline constexpr std::uint32_t kShadowCascades = 4;

struct ShadowSettings {
    bool enabled = true;
    std::uint32_t resolution = 2048; ///< per cascade
    float max_distance = 150.0f;     ///< metres from the camera
    float split_lambda = 0.75f;      ///< 0: linear splits, 1: logarithmic
};

struct ShadowCascades {
    std::array<glm::mat4, kShadowCascades> view_projection{}; ///< world -> cascade clip space
    std::array<float, kShadowCascades> split_far{};           ///< view depth where each cascade ends
    std::array<float, kShadowCascades> texel_size{};          ///< world size of one texel
    std::array<glm::vec4, kShadowCascades> spheres{};         ///< world centre (snapped), radius
};

/// Cascade boundaries in view depth: kShadowCascades + 1 values from near to far.
[[nodiscard]] std::array<float, kShadowCascades + 1> cascade_splits(float near_plane, float far_plane, float lambda);

/// World -> light-space rotation for a light shining from `towards_light` (unit).
[[nodiscard]] glm::mat4 light_rotation(const glm::vec3& towards_light) noexcept;

/// The cascades of `view` for a directional light.
[[nodiscard]] ShadowCascades compute_cascades(const View& view, const glm::vec3& towards_light,
                                              const ShadowSettings& settings);

} // namespace f3d::render::raster
