// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/shadows.hpp"

#include <algorithm>
#include <cmath>

namespace f3d::render::raster {

std::array<float, kShadowCascades + 1> cascade_splits(float near_plane, float far_plane, float lambda) {
    std::array<float, kShadowCascades + 1> out{};
    near_plane = std::max(near_plane, 1e-3f);
    far_plane = std::max(far_plane, near_plane * 1.001f);
    for (std::uint32_t i = 0; i <= kShadowCascades; ++i) {
        const float f = static_cast<float>(i) / kShadowCascades;
        const float log_split = near_plane * std::pow(far_plane / near_plane, f);
        const float linear_split = near_plane + (far_plane - near_plane) * f;
        out[i] = lambda * log_split + (1.0f - lambda) * linear_split;
    }
    out[0] = near_plane;
    out[kShadowCascades] = far_plane;
    return out;
}

glm::mat4 light_rotation(const glm::vec3& towards_light) noexcept {
    const glm::vec3 forward = -glm::normalize(towards_light);
    const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::lookAtRH(glm::vec3(0.0f), forward, up);
}

ShadowCascades compute_cascades(const View& view, const glm::vec3& towards_light, const ShadowSettings& settings) {
    ShadowCascades out;
    const float far_plane = std::min(view.far_plane, settings.max_distance);
    const auto splits = cascade_splits(view.near_plane, far_plane, settings.split_lambda);
    const glm::mat4 camera_world = glm::inverse(view.view);
    const glm::vec3 camera = glm::vec3(camera_world[3]);
    const glm::vec3 forward = -glm::normalize(glm::vec3(camera_world[2]));
    const glm::mat4 rotation = light_rotation(towards_light);
    const float resolution = static_cast<float>(std::max(settings.resolution, 1u));

    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        const float n = splits[i];
        const float f = splits[i + 1];
        // Bounding sphere of the slice [n, f], centred on the view axis. Its radius
        // depends only on the projection, so turning the camera doesn't resize it.
        float centre_depth = 0.0f;
        float radius = 0.0f;
        if (view.orthographic) {
            const float half_w = 1.0f / view.projection[0][0];
            const float half_h = 1.0f / view.projection[1][1];
            centre_depth = 0.5f * (n + f);
            radius = std::sqrt(0.25f * (f - n) * (f - n) + half_w * half_w + half_h * half_h);
        } else {
            const float tan_y = 1.0f / view.projection[1][1];
            const float tan_x = 1.0f / view.projection[0][0];
            const float k2 = tan_x * tan_x + tan_y * tan_y; // squared half-diagonal slope
            centre_depth = 0.5f * (n + f) * (1.0f + k2);
            if (centre_depth >= f) {
                centre_depth = f;
                radius = f * std::sqrt(k2);
            } else {
                radius = std::sqrt((f - centre_depth) * (f - centre_depth) + f * f * k2);
            }
        }
        // Quantised so rounding noise never changes the projection between frames.
        radius = std::ceil(radius * 16.0f) / 16.0f;
        const float texel = 2.0f * radius / resolution;

        // Snap the light-space centre to whole texels (depth too, so a move along the
        // light keeps the matrix).
        const glm::vec3 centre = camera + forward * centre_depth;
        glm::vec3 ls = glm::vec3(rotation * glm::vec4(centre, 1.0f));
        ls = glm::floor(ls / texel + 0.5f) * texel;
        // Casters in front of the near plane are clamped onto it (depth clip off).
        const glm::mat4 projection =
            glm::orthoRH_ZO(ls.x - radius, ls.x + radius, ls.y - radius, ls.y + radius, -ls.z - radius, -ls.z + radius);
        out.view_projection[i] = projection * rotation;
        out.split_far[i] = f;
        out.texel_size[i] = texel;
        out.spheres[i] = glm::vec4(glm::vec3(glm::inverse(rotation) * glm::vec4(ls, 1.0f)), radius);
    }
    return out;
}

} // namespace f3d::render::raster
