// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/math.hpp"
#include "f3d/render/raster/lights.hpp"
#include "f3d/render/raster/shadows.hpp"
#include "f3d/render/view.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace f3d::render::raster {

/// The engine-independent view (f3d/render/view.hpp).
using View = render::View;

/// GPU layout of shaders/view.hlsli `ViewConstants`.
struct ViewConstants {
    glm::mat4 view_projection;
    glm::mat4 inverse_view_projection;
    glm::mat4 view;
    glm::vec4 camera_position; ///< w: 1 if orthographic
    glm::vec4 viewport;        ///< width, height, 1/width, 1/height
    glm::vec4 grid_params;     ///< fade start, fade end, minor spacing, major spacing
    glm::vec4
        frame_params; ///< x: exposure scale, y: specular environment mips, z: 1 = times the GPU exposure, w: view mode
    glm::mat4 inverse_projection;
    glm::vec4 cluster_params; ///< tile width, tile height (pixels), depth slice scale, bias
    glm::uvec4 light_counts;  ///< x: local lights, y: directional lights, z: shadowing directional light + 1 (0: none)
    glm::vec4 directional_direction[kMaxDirectionalLights]; ///< towards the light
    glm::vec4 directional_color[kMaxDirectionalLights];     ///< illuminance
    glm::mat4 shadow_matrices[kShadowCascades];             ///< world -> cascade clip space
    glm::vec4 shadow_splits;                                ///< view depth where each cascade ends
    glm::vec4 shadow_texel;                                 ///< world size of a shadow texel per cascade
};
static_assert(sizeof(ViewConstants) ==
              4 * 64 + 4 * 16 + 2 * 16 + 2 * kMaxDirectionalLights * 16 + kShadowCascades * 64 + 2 * 16);

/// Lighting without a Sky entity: a default sun used when the scene has no
/// directional light, and a hemisphere environment (sky and ground radiance).
/// Exposure is 1 then: an illuminance of pi lights a white Lambert surface facing
/// the light to 1.
struct SimpleLighting {
    glm::vec3 sun_direction = glm::normalize(glm::vec3(0.45f, 0.8f, 0.35f)); ///< towards the sun
    glm::vec3 sun_color{2.98f, 2.89f, 2.70f};                                ///< illuminance
    float ambient = 0.5f;
    glm::vec3 sky_color{0.34f, 0.38f, 0.46f};    ///< radiance from above (times ambient)
    glm::vec3 ground_color{0.14f, 0.12f, 0.10f}; ///< radiance from below (times ambient)
};

/// Ground-grid settings (metres).
struct GridSettings {
    bool visible = true;
    float minor_spacing = 1.0f;
    float major_spacing = 10.0f;
};

/// The lights, shadows and exposure of one frame.
struct FrameLighting {
    std::vector<DirectionalLight> directional; ///< at most kMaxDirectionalLights
    std::int32_t shadow_light = -1;            ///< index into `directional` with shadows, -1: none
    float exposure = 1.0f;                     ///< the CPU's exposure: metered from the Sky (seeds the GPU meter), or 1
    float exposure_scale = 1.0f; ///< what the shaders multiply radiance by (times the GPU exposure if gpu_exposure)
    bool gpu_exposure = false;   ///< the automatic exposure (histogram) applies
    std::uint32_t view_mode = 0; ///< ViewMode
    ShadowCascades cascades;
    std::uint32_t local_lights = 0;
};

/// The view's constants for a frame (an empty `frame`: no lights, exposure 1).
[[nodiscard]] ViewConstants make_view_constants(const View& view, const GridSettings& grid,
                                                const FrameLighting& frame = {});

} // namespace f3d::render::raster
