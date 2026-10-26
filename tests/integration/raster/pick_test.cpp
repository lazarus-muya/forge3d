// SPDX-License-Identifier: Apache-2.0
// Renders a known scene into the ID buffer and checks that reading ids at fixed
// pixels returns the expected entities (phases.md P2.4 picking test).
#include <doctest/doctest.h>

#include "f3d/assets/mesh_library.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/render/raster/view.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

namespace rhi = f3d::render::rhi;

namespace {

void pick_check(rhi::Backend backend) {
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
    f3d::render::raster::RasterEngine renderer(device, shaders);

    f3d::scene::World world;
    f3d::assets::MeshLibrary meshes;
    f3d::assets::MeshData mesh;
    mesh.name = "tri";
    mesh.vertices = {
        {glm::vec3{-1.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0xFFFFFFFFu},
        {glm::vec3{-1.0f, -1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0xFFFFFFFFu},
        {glm::vec3{1.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, 0xFFFFFFFFu},
    };
    mesh.indices = {0, 1, 2};
    meshes.add("tri", mesh);
    const entt::entity e = world.create("tri");
    world.registry().emplace<f3d::scene::MeshRenderer>(e, f3d::scene::MeshRenderer{{f3d::core::Guid{}, "tri"}, true});
    const f3d::render::RenderScene scene{&world, &meshes};

    f3d::render::raster::View view;
    view.view = glm::mat4(1.0f);
    view.projection = glm::mat4(1.0f);
    view.width = 64;
    view.height = 64;

    // The triangle covers the upper-left half: NDC x <= y (y up), so pixel (12, 6)
    // is inside and (52, 52) is not.
    INFO("backend ", rhi::to_string(backend));
    CHECK((renderer.pick(view, scene, 12, 6) == e));
    CHECK((renderer.pick(view, scene, 52, 52) == entt::null));

    const std::vector<entt::entity> region = renderer.pick_region(view, scene, 0, 0, 64, 64);
    REQUIRE(region.size() == 1);
    CHECK(region[0] == e);

    // A second triangle behind the first (depth 0.5 vs 0): the front one wins...
    world.set_local(e, glm::vec3{0.0f, 0.0f, 0.1f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});
    const entt::entity back = world.create("back");
    world.registry().emplace<f3d::scene::MeshRenderer>(back,
                                                       f3d::scene::MeshRenderer{{f3d::core::Guid{}, "tri"}, true});
    world.set_local(back, glm::vec3{0.0f, 0.0f, 0.5f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});
    CHECK((renderer.pick(view, scene, 12, 6) == e));

    // ...until its layer is locked (P2.5): locked entities are skipped, so the one behind is picked.
    const f3d::core::Guid layer = f3d::core::Guid::generate();
    REQUIRE(world.layers().insert({layer, "Locked", true, true}));
    world.registry().emplace<f3d::scene::Layer>(e, f3d::scene::Layer{layer});
    CHECK((renderer.pick(view, scene, 12, 6) == back));
    const std::vector<entt::entity> locked_region = renderer.pick_region(view, scene, 0, 0, 64, 64);
    REQUIRE(locked_region.size() == 1);
    CHECK(locked_region[0] == back);

    // Hidden layers are skipped too; with both entities hidden nothing is picked.
    world.layers().find(layer)->locked = false;
    world.layers().find(layer)->visible = false;
    world.registry().emplace<f3d::scene::Layer>(back, f3d::scene::Layer{layer});
    CHECK((renderer.pick(view, scene, 12, 6) == entt::null));
}

} // namespace

TEST_CASE("raster: picking returns the entity under the cursor (D3D12)") {
    if (!rhi::is_backend_available(rhi::Backend::D3D12)) {
        return;
    }
    pick_check(rhi::Backend::D3D12);
}

TEST_CASE("raster: picking returns the entity under the cursor (Vulkan)") {
    if (!rhi::is_backend_available(rhi::Backend::Vulkan)) {
        return;
    }
    pick_check(rhi::Backend::Vulkan);
}
