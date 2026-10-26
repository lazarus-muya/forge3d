// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/material_preview.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/scene/components.hpp"

#include <algorithm>

namespace f3d::render::raster {
namespace {
constexpr const char* kSphere = "preview#mesh/0";
constexpr const char* kMaterial = "preview#material";
} // namespace

MaterialPreview::MaterialPreview(rhi::Device& device, rhi::ShaderLibrary& shaders)
    : device_(device), engine_(device, shaders) {
    engine_.grid().visible = false;
    engine_.set_debug_lines(false);
    engine_.shadow_settings().resolution = 512;
    meshes_.add(kSphere, assets::make_uv_sphere(1.0f, 64, 32));
    const entt::entity sphere = world_.create("Sphere");
    scene::MeshRenderer renderer;
    renderer.mesh = {core::Guid{}, kSphere};
    renderer.materials = {{core::Guid{}, kMaterial}};
    world_.registry().emplace<scene::MeshRenderer>(sphere, std::move(renderer));
}

const rhi::Texture& MaterialPreview::render(const assets::Material& material, std::uint32_t size) {
    F3D_ZONE;
    size = std::max(size, 8u);
    if (!output_ || output_.width() != size) {
        output_ = device_.create_texture({"Material preview", size, size, RasterEngine::display_format(),
                                          rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
    }
    meshes_.add_material(kMaterial, material);

    View view;
    const float fov = glm::radians(30.0f);
    view.camera_position = glm::vec3(0.0f, 0.0f, 4.2f);
    view.view = glm::lookAtRH(view.camera_position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    view.near_plane = 1.0f;
    view.far_plane = 10.0f;
    view.projection = glm::perspectiveRH_ZO(fov, 1.0f, view.near_plane, view.far_plane);
    view.width = size;
    view.height = size;
    engine_.render(device_.context(), view, {&world_, &meshes_}, {output_, {0.10f, 0.11f, 0.13f, 1.0f}});
    return output_;
}

} // namespace f3d::render::raster
