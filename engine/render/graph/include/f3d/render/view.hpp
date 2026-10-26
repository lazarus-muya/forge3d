// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/math.hpp"

#include <cmath>
#include <cstdint>

namespace f3d::render {

/// A camera's view of the scene for one frame.
struct View {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 camera_position{0.0f};
    std::uint32_t width = 1; ///< render target size in pixels
    std::uint32_t height = 1;
    bool orthographic = false;
    float near_plane = 0.05f;
    float far_plane = 5000.0f;

    [[nodiscard]] glm::mat4 view_projection() const noexcept { return projection * view; }

    /// The world-space ray through pixel (x, y) (pixel centres at +0.5, y down).
    /// `direction` is normalised. Works for both projections.
    void pixel_ray(float x, float y, glm::vec3& origin, glm::vec3& direction) const noexcept {
        const float ndc_x = 2.0f * x / static_cast<float>(width) - 1.0f;
        const float ndc_y = 1.0f - 2.0f * y / static_cast<float>(height);
        const glm::mat4 inverse = glm::inverse(view_projection());
        const glm::vec4 near_point = inverse * glm::vec4(ndc_x, ndc_y, 0.0f, 1.0f); // depth 0..1 (ZO)
        const glm::vec4 far_point = inverse * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
        origin = glm::vec3(near_point) / near_point.w;
        direction = glm::normalize(glm::vec3(far_point) / far_point.w - origin);
    }

    /// Angle (radians) subtended by `pixels` at the centre of the view (perspective),
    /// or 0 for orthographic views (use world-space distances there).
    [[nodiscard]] float pixel_angle(float pixels) const noexcept {
        if (orthographic) {
            return 0.0f;
        }
        const float tan_half_fov = 1.0f / projection[1][1];
        return std::atan(2.0f * tan_half_fov * pixels / static_cast<float>(height));
    }
};

} // namespace f3d::render
