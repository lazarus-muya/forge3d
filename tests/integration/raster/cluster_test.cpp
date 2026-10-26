// SPDX-License-Identifier: Apache-2.0
// The light-clustering compute pass agrees with the CPU reference (phases.md P4.2),
// and lights light the scene.
#include <doctest/doctest.h>

#include "f3d/assets/mesh_library.hpp"
#include "f3d/render/raster/lights.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <cstring>
#include <random>

using namespace f3d;
namespace rhi = f3d::render::rhi;
using namespace f3d::render::raster;

namespace {

void cluster_check(rhi::Backend backend) {
    rhi::DeviceDesc desc;
    desc.backend = backend;
    desc.allow_fallback = false;
    auto created = rhi::Device::create(desc);
    if (!created.device) {
        MESSAGE("skipping ", rhi::to_string(backend), ": ", created.error);
        return;
    }
    rhi::Device& device = *created.device;
    rhi::ShaderLibrary shaders(device, rhi::default_shader_library_desc());
    RasterEngine engine(device, shaders);
    engine.grid().visible = false;
    engine.set_debug_lines(false);

    scene::World world;
    assets::MeshLibrary meshes;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> pos(-10.0f, 10.0f), range(0.5f, 5.0f);
    for (int i = 0; i < 300; ++i) {
        const entt::entity e = world.create("Light");
        scene::Light light;
        light.type = i % 3 == 0 ? scene::LightType::Spot : scene::LightType::Point;
        light.range = range(rng);
        world.registry().emplace<scene::Light>(e, light);
        world.set_local(e, glm::vec3(pos(rng), pos(rng) * 0.3f + 1.0f, pos(rng) - 5.0f),
                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    }

    render::View view;
    view.camera_position = glm::vec3(0.0f, 3.0f, 12.0f);
    view.view = glm::lookAtRH(view.camera_position, glm::vec3(0.0f, 0.0f, -4.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    view.width = 640;
    view.height = 360;
    view.near_plane = 0.1f;
    view.far_plane = 200.0f;
    view.projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 640.0f / 360.0f, view.near_plane, view.far_plane);

    const rhi::Texture output =
        device.create_texture({"Output", view.width, view.height, RasterEngine::display_format(),
                               rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
    engine.render(device.context(), view, {&world, &meshes}, {output, {0.0f, 0.0f, 0.0f, 1.0f}});
    CHECK(engine.last_local_lights() == 300);

    const auto counts_bytes = device.context().read_buffer(engine.cluster_counts());
    const auto indices_bytes = device.context().read_buffer(engine.cluster_indices());
    REQUIRE(counts_bytes.size() == kClusterCount * 4);
    REQUIRE(indices_bytes.size() == kClusterCount * kMaxLightsPerCluster * 4);
    std::vector<std::uint32_t> counts(kClusterCount), indices(kClusterCount * kMaxLightsPerCluster);
    std::memcpy(counts.data(), counts_bytes.data(), counts_bytes.size());
    std::memcpy(indices.data(), indices_bytes.data(), indices_bytes.size());

    const auto reference = assign_lights(view, gather_lights(world).local);
    std::size_t mismatches = 0;
    std::size_t total = 0;
    for (std::uint32_t c = 0; c < kClusterCount; ++c) {
        const std::vector<std::uint32_t> gpu(indices.begin() + c * kMaxLightsPerCluster,
                                             indices.begin() + c * kMaxLightsPerCluster + counts[c]);
        total += reference[c].size();
        if (gpu != reference[c]) {
            ++mismatches;
        }
    }
    INFO("backend ", rhi::to_string(backend), ": ", mismatches, " of ", kClusterCount, " clusters differ");
    CHECK(total > 500);     // the scene actually exercises the grid
    CHECK(mismatches <= 2); // float rounding at a sphere/box boundary at most
}

void lighting_check(rhi::Backend backend) {
    rhi::DeviceDesc desc;
    desc.backend = backend;
    desc.allow_fallback = false;
    auto created = rhi::Device::create(desc);
    if (!created.device) {
        return;
    }
    rhi::Device& device = *created.device;
    rhi::ShaderLibrary shaders(device, rhi::default_shader_library_desc());
    RasterEngine engine(device, shaders);
    engine.grid().visible = false;
    engine.set_debug_lines(false);
    engine.lighting().sun_color = glm::vec3(0.0f); // dark except for the lights
    engine.lighting().ambient = 0.0f;
    engine.settings().tonemapper = Tonemapper::None; // the light's own colour, not AgX's highlight roll-off

    scene::World world;
    assets::MeshLibrary meshes;
    assets::MeshData plane;
    plane.vertices = {{glm::vec3(-5, 0, -5), glm::vec3(0, 1, 0), 0xFFFFFFFFu},
                      {glm::vec3(-5, 0, 5), glm::vec3(0, 1, 0), 0xFFFFFFFFu},
                      {glm::vec3(5, 0, 5), glm::vec3(0, 1, 0), 0xFFFFFFFFu},
                      {glm::vec3(5, 0, -5), glm::vec3(0, 1, 0), 0xFFFFFFFFu}};
    plane.indices = {0, 1, 2, 0, 2, 3};
    meshes.add("plane", plane);
    const entt::entity ground = world.create("Ground");
    world.registry().emplace<scene::MeshRenderer>(ground, scene::MeshRenderer{{core::Guid{}, "plane"}, true});
    // A red point light over the left half.
    const entt::entity lamp = world.create("Lamp");
    world.registry().emplace<scene::Light>(
        lamp, scene::Light{scene::LightType::Point, glm::vec3(1.0f, 0.0f, 0.0f), 20.0f, 3.0f});
    world.set_local(lamp, glm::vec3(-2.5f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

    render::View view;
    view.camera_position = glm::vec3(0.0f, 10.0f, 0.01f);
    view.view = glm::lookAtRH(view.camera_position, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    view.width = view.height = 128;
    view.near_plane = 0.1f;
    view.far_plane = 50.0f;
    view.projection = glm::perspectiveRH_ZO(glm::radians(70.0f), 1.0f, view.near_plane, view.far_plane);
    const rhi::Texture output = device.create_texture({"Output", 128, 128, RasterEngine::display_format(),
                                                       rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
    engine.render(device.context(), view, {&world, &meshes}, {output, {0.0f, 0.0f, 0.0f, 1.0f}});
    const core::Image image = device.context().read_texture(output);
    REQUIRE(image.width == 128);
    const auto pixel = [&](int x, int y) { return &image.rgba[(static_cast<std::size_t>(y) * 128 + x) * 4]; };
    // Under the lamp: red; far right (beyond its 3 m range): black.
    const std::uint8_t* lit = pixel(128 / 2 - 23, 64);
    const std::uint8_t* dark = pixel(100, 64); // ~4 m right of the centre, 6.5 m from the lamp
    INFO("backend ", rhi::to_string(backend), " lit ", int(lit[0]), ",", int(lit[1]), " dark ", int(dark[0]));
    CHECK(lit[0] > 100);
    CHECK(lit[1] < 10);
    CHECK(dark[0] < 5);
}

} // namespace

TEST_CASE("raster: light clustering on the GPU matches the CPU reference (D3D12)") {
    if (rhi::is_backend_available(rhi::Backend::D3D12)) {
        cluster_check(rhi::Backend::D3D12);
    }
}

TEST_CASE("raster: light clustering on the GPU matches the CPU reference (Vulkan)") {
    if (rhi::is_backend_available(rhi::Backend::Vulkan)) {
        cluster_check(rhi::Backend::Vulkan);
    }
}

TEST_CASE("raster: point lights light what is in range (D3D12, Vulkan)") {
    for (const auto backend : {rhi::Backend::D3D12, rhi::Backend::Vulkan}) {
        if (rhi::is_backend_available(backend)) {
            lighting_check(backend);
        }
    }
}
