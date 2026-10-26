// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/thumbnail_renderer.hpp"

#include "f3d/assets/mesh_library.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <cmath>

namespace f3d::render::raster {

ThumbnailRenderer::ThumbnailRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders)
    : device_(device), renderer_(device, shaders) {
    renderer_.grid().visible = false;
    renderer_.set_debug_lines(false);
    renderer_.shadow_settings().resolution = 512; // small views need little shadow detail
}

void ThumbnailRenderer::ensure_targets(std::uint32_t size) {
    if (size == size_ && output_) {
        return;
    }
    size_ = size;
    output_ = device_.create_texture({"Thumbnail", size, size, RasterEngine::display_format(),
                                      rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
}

core::Image ThumbnailRenderer::render(const assets::ModelScene& model, std::uint32_t size) {
    F3D_ZONE;
    ensure_targets(std::max(size, 8u));
    // A throwaway scene holding just the model.
    scene::World world;
    assets::MeshLibrary meshes;
    for (std::size_t i = 0; i < model.meshes.size(); ++i) {
        meshes.add(assets::gltf_mesh_path("thumbnail", i), model.meshes[i]);
    }
    assets::spawn_gltf(world, model, "thumbnail");
    core::Bounds bounds = assets::world_bounds(world, meshes);
    if (bounds.empty()) {
        bounds.add(glm::vec3(-1.0f));
        bounds.add(glm::vec3(1.0f));
    }

    // A three-quarter view, as close as possible with every corner of the bounds
    // inside the frame (with a margin): for a corner at view-space (x, y) and depth
    // offset z towards the camera, |x| <= (d - z) * tan(fov / 2) gives d.
    const float fov = glm::radians(30.0f);
    const float fit = std::tan(fov * 0.5f) * 0.9f;
    const glm::vec3 centre = bounds.center();
    const glm::vec3 direction = glm::normalize(glm::vec3(0.6f, 0.45f, 0.75f)); // from the target towards the eye
    const glm::vec3 right = glm::normalize(glm::cross(-direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, -direction);
    const float size_hint = std::max(glm::length(bounds.extent()), 1e-3f);
    float distance = 0.0f;
    float nearest = 0.0f;  // largest z (closest to the camera)
    float farthest = 0.0f; // smallest z
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? bounds.max.x : bounds.min.x, (i & 2) ? bounds.max.y : bounds.min.y,
                               (i & 4) ? bounds.max.z : bounds.min.z);
        const glm::vec3 offset = corner - centre;
        const float x = std::abs(glm::dot(offset, right));
        const float y = std::abs(glm::dot(offset, up));
        const float z = glm::dot(offset, direction);
        distance = std::max(distance, z + std::max(x, y) / fit);
        nearest = i == 0 ? z : std::max(nearest, z);
        farthest = i == 0 ? z : std::min(farthest, z);
    }
    distance = std::max(distance, nearest + size_hint * 0.05f);
    View view;
    view.camera_position = centre + direction * distance;
    view.view = glm::lookAtRH(view.camera_position, centre, glm::vec3(0.0f, 1.0f, 0.0f));
    view.near_plane = std::max((distance - nearest) * 0.5f, size_hint * 1e-4f);
    view.far_plane = (distance - farthest) * 1.5f + size_hint;
    view.projection = glm::perspectiveRH_ZO(fov, 1.0f, view.near_plane, view.far_plane);
    view.width = size_;
    view.height = size_;

    rhi::CommandContext& ctx = device_.context();
    renderer_.render(ctx, view, {&world, &meshes}, {output_, {0.16f, 0.17f, 0.19f, 1.0f}});
    return ctx.read_texture(output_);
}

} // namespace f3d::render::raster
