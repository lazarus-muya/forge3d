// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/lights.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <cmath>

namespace f3d::render::raster {

SceneLights gather_lights(scene::World& world) {
    F3D_ZONE;
    SceneLights out;
    world.update_transforms();
    // Scene order, so the light indices don't depend on ECS storage order.
    for (const entt::entity e : world.depth_first()) {
        const auto* light = world.registry().try_get<scene::Light>(e);
        if (!light || !scene::is_visible(world, e)) {
            continue;
        }
        const glm::mat4& m = world.transform(e).world;
        const glm::vec3 forward = glm::normalize(-glm::vec3(m[2])); // local -Z
        const glm::vec3 color = light->color * light->intensity;
        if (light->type == scene::LightType::Directional) {
            if (out.directional.size() < kMaxDirectionalLights) {
                out.directional.push_back({-forward, color, light->cast_shadows, light->volumetric_scattering});
            }
            continue;
        }
        if (out.local.size() >= kMaxLocalLights || light->range <= 0.0f) {
            continue;
        }
        GpuLight g;
        g.position = glm::vec3(m[3]);
        g.range = light->range;
        g.color = color;
        g.direction = forward;
        g.volumetric = std::max(light->volumetric_scattering, 0.0f);
        if (light->type == scene::LightType::Spot) {
            g.type = 1;
            const float outer = std::cos(std::max(light->outer_cone, 0.0f));
            const float inner = std::cos(std::clamp(light->inner_cone, 0.0f, std::max(light->outer_cone, 0.0f)));
            g.spot_scale = 1.0f / std::max(inner - outer, 1e-4f);
            g.spot_offset = -outer * g.spot_scale;
        }
        out.local.push_back(g);
    }
    return out;
}

ClusterDepth cluster_depth(const View& view) noexcept {
    ClusterDepth d;
    d.near_plane = std::max(view.near_plane, 1e-4f);
    d.far_plane = std::max(view.far_plane, d.near_plane * 1.001f);
    const float log_ratio = std::log(d.far_plane / d.near_plane);
    d.scale = static_cast<float>(kClusterSlices) / log_ratio;
    d.bias = -static_cast<float>(kClusterSlices) * std::log(d.near_plane) / log_ratio;
    return d;
}

std::uint32_t cluster_slice(float view_depth, const ClusterDepth& depth) noexcept {
    const float slice = std::floor(std::log(std::max(view_depth, 1e-6f)) * depth.scale + depth.bias);
    return static_cast<std::uint32_t>(std::clamp(slice, 0.0f, static_cast<float>(kClusterSlices - 1)));
}

glm::uvec2 cluster_tile_size(const View& view) noexcept {
    return {(std::max(view.width, 1u) + kClusterTilesX - 1) / kClusterTilesX,
            (std::max(view.height, 1u) + kClusterTilesY - 1) / kClusterTilesY};
}

std::uint32_t cluster_at(const View& view, float px, float py, float view_depth) noexcept {
    const glm::uvec2 tile = cluster_tile_size(view);
    const auto x = std::min(static_cast<std::uint32_t>(std::max(px, 0.0f)) / tile.x, kClusterTilesX - 1);
    const auto y = std::min(static_cast<std::uint32_t>(std::max(py, 0.0f)) / tile.y, kClusterTilesY - 1);
    return cluster_index(x, y, cluster_slice(view_depth, cluster_depth(view)));
}

Aabb cluster_bounds(const View& view, std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    const glm::uvec2 tile = cluster_tile_size(view);
    const ClusterDepth depth = cluster_depth(view);
    const float d0 = std::exp((static_cast<float>(z) - depth.bias) / depth.scale);
    const float d1 = std::exp((static_cast<float>(z + 1) - depth.bias) / depth.scale);
    const glm::mat4 inverse = glm::inverse(view.projection);
    const auto unproject = [&](float ndc_x, float ndc_y, float ndc_z) {
        const glm::vec4 p = inverse * glm::vec4(ndc_x, ndc_y, ndc_z, 1.0f);
        return glm::vec3(p) / p.w;
    };
    Aabb box{glm::vec3(1e30f), glm::vec3(-1e30f)};
    const float w = static_cast<float>(std::max(view.width, 1u));
    const float h = static_cast<float>(std::max(view.height, 1u));
    for (int corner = 0; corner < 4; ++corner) {
        const float px = static_cast<float>((x + (corner & 1)) * tile.x);
        const float py = static_cast<float>((y + ((corner >> 1) & 1)) * tile.y);
        const float ndc_x = 2.0f * px / w - 1.0f;
        const float ndc_y = 1.0f - 2.0f * py / h;
        // The line through this corner from the near to the far plane (a ray from the
        // eye in perspective, parallel to -Z in orthographic views), cut at d0 and d1.
        const glm::vec3 pn = unproject(ndc_x, ndc_y, 0.0f);
        const glm::vec3 pf = unproject(ndc_x, ndc_y, 1.0f);
        for (const float d : {d0, d1}) {
            const float t = (d + pn.z) / (pn.z - pf.z); // -z is the depth
            const glm::vec3 p = pn + t * (pf - pn);
            box.min = glm::min(box.min, p);
            box.max = glm::max(box.max, p);
        }
    }
    return box;
}

bool sphere_intersects(const Aabb& box, const glm::vec3& centre, float radius) noexcept {
    const glm::vec3 closest = glm::clamp(centre, box.min, box.max);
    const glm::vec3 d = closest - centre;
    return glm::dot(d, d) <= radius * radius;
}

std::vector<std::vector<std::uint32_t>> assign_lights(const View& view, std::span<const GpuLight> lights) {
    std::vector<std::vector<std::uint32_t>> out(kClusterCount);
    std::vector<glm::vec3> centres;
    centres.reserve(lights.size());
    for (const GpuLight& l : lights) {
        centres.push_back(glm::vec3(view.view * glm::vec4(l.position, 1.0f)));
    }
    for (std::uint32_t z = 0; z < kClusterSlices; ++z) {
        for (std::uint32_t y = 0; y < kClusterTilesY; ++y) {
            for (std::uint32_t x = 0; x < kClusterTilesX; ++x) {
                const Aabb box = cluster_bounds(view, x, y, z);
                auto& list = out[cluster_index(x, y, z)];
                for (std::uint32_t i = 0; i < lights.size() && list.size() < kMaxLightsPerCluster; ++i) {
                    if (sphere_intersects(box, centres[i], lights[i].range)) {
                        list.push_back(i);
                    }
                }
            }
        }
    }
    return out;
}

} // namespace f3d::render::raster
