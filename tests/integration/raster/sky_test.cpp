// SPDX-License-Identifier: Apache-2.0
// Sky, image-based lighting, reflection probes and shadows on the GPU (phases.md P4.3).
#include <doctest/doctest.h>

#include "f3d/assets/mesh.hpp"
#include "f3d/assets/mesh_library.hpp"
#include "f3d/render/raster/atmosphere.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/sky.hpp"
#include "f3d/scene/world.hpp"

#include <cmath>
#include <cstring>
#include <memory>

using namespace f3d;
namespace rhi = f3d::render::rhi;
using namespace f3d::render::raster;

namespace {

struct Gpu {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::ShaderLibrary> shaders;
    rhi::Texture output;
};

Gpu make_gpu(rhi::Backend backend) {
    rhi::DeviceDesc desc;
    desc.backend = backend;
    desc.allow_fallback = false;
    auto created = rhi::Device::create(desc);
    Gpu gpu;
    if (!created.device) {
        MESSAGE("skipping ", rhi::to_string(backend), ": ", created.error);
        return gpu;
    }
    gpu.device = std::move(created.device);
    gpu.shaders = std::make_unique<rhi::ShaderLibrary>(*gpu.device, rhi::default_shader_library_desc());
    gpu.output = gpu.device->create_texture({"Output", 320, 180, RasterEngine::display_format(),
                                             rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
    return gpu;
}

render::View make_view(glm::vec3 eye, glm::vec3 target) {
    render::View view;
    view.camera_position = eye;
    view.view = glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    view.width = 320;
    view.height = 180;
    view.near_plane = 0.1f;
    view.far_plane = 500.0f;
    view.projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 320.0f / 180.0f, view.near_plane, view.far_plane);
    return view;
}

using Sh = std::array<glm::vec3, 9>;

Sh read_sh(rhi::Device& device, RasterEngine& engine, std::uint32_t slot) {
    const auto bytes = device.context().read_buffer(engine.sky().sh_buffer());
    std::vector<glm::vec4> c(bytes.size() / sizeof(glm::vec4));
    std::memcpy(c.data(), bytes.data(), c.size() * sizeof(glm::vec4));
    Sh out{};
    for (std::uint32_t i = 0; i < 9; ++i) {
        out[i] = glm::vec3(c[slot * 9 + i]);
    }
    return out;
}

/// Irradiance for normal n (the shader's evaluation).
glm::vec3 irradiance(const Sh& c, glm::vec3 n) {
    const float b[9] = {0.282095f,
                        0.488603f * n.y,
                        0.488603f * n.z,
                        0.488603f * n.x,
                        1.092548f * n.x * n.y,
                        1.092548f * n.y * n.z,
                        0.315392f * (3.0f * n.z * n.z - 1.0f),
                        1.092548f * n.x * n.z,
                        0.546274f * (n.x * n.x - n.y * n.y)};
    glm::vec3 e(0.0f);
    for (std::size_t i = 0; i < 9; ++i) {
        e += c[i] * b[i];
    }
    return e;
}

glm::vec3 sky_view_texel(rhi::Device& device, RasterEngine& engine, std::uint32_t x, std::uint32_t y) {
    const auto lut = device.context().read_texture_float(engine.sky().sky_view_lut());
    const std::size_t i = (static_cast<std::size_t>(y) * engine.sky().sky_view_lut().width() + x) * 4;
    return glm::vec3(lut[i], lut[i + 1], lut[i + 2]) / kEnvScale;
}

void sky_checks(rhi::Backend backend) {
    Gpu gpu = make_gpu(backend);
    if (!gpu.device) {
        return;
    }
    RasterEngine engine(*gpu.device, *gpu.shaders);
    scene::World world;
    assets::MeshLibrary meshes;
    const entt::entity e = world.create("Sky");
    world.registry().emplace<scene::Sky>(e);
    const render::View view = make_view({0.0f, 2.0f, 0.0f}, {0.0f, 2.0f, -1.0f});
    const auto render_at = [&](float hours) {
        world.registry().get<scene::Sky>(e).time_of_day = hours;
        engine.render(gpu.device->context(), view, {&world, &meshes}, {gpu.output, {}});
        return world.registry().get<scene::Sky>(e);
    };

    // Transmittance LUT == the CPU reference (straight up and towards the horizon).
    render_at(12.0f);
    const auto lut = gpu.device->context().read_texture_float(engine.sky().transmittance_lut());
    REQUIRE(lut.size() == 256u * 64u * 4u);
    const AtmosphereParams a = atmosphere_params(world.registry().get<scene::Sky>(e));
    // Bottom row (r = ground), first column (d = d_min: straight up).
    const glm::vec3 up_gpu(lut[0], lut[1], lut[2]);
    const glm::vec3 up_cpu = transmittance(a, 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) {
        CHECK(up_gpu[c] == doctest::Approx(up_cpu[c]).epsilon(0.03));
    }

    // At noon the environment's irradiance from above is the CPU single-scattering
    // estimate plus multiple scattering (more, but not wildly so), the zenith is blue.
    for (const float hours : {12.0f, 18.0f}) {
        const scene::Sky sky = render_at(hours);
        const SkyLighting cpu = sky_lighting(sky, scene::celestial(sky), a, 2.0f);
        const glm::vec3 e_up = irradiance(read_sh(*gpu.device, engine, 0), {0.0f, 1.0f, 0.0f});
        CHECK(e_up.g > cpu.sky_illuminance.g);
        CHECK(e_up.g < cpu.sky_illuminance.g * 2.0f);
        const glm::vec3 zenith = sky_view_texel(*gpu.device, engine, 96, engine.sky().sky_view_lut().height() - 1);
        CHECK(zenith.b > zenith.g);
        CHECK(zenith.g > zenith.r);
        CHECK(engine.frame_lighting().exposure == doctest::Approx(cpu.exposure));
        // The sun leads the directional lights and casts shadows, but with nothing in
        // the scene to cast them the shadow pass is skipped.
        REQUIRE_FALSE(engine.frame_lighting().directional.empty());
        CHECK(engine.frame_lighting().directional[0].cast_shadows);
        CHECK(engine.frame_lighting().shadow_light == -1);
    }

    // Morning: the sun is in the east (+X), so the sky is brighter on that side and
    // the environment's SH leans towards +X (the lit ground below evens it out a bit).
    render_at(7.0f);
    const Sh morning = read_sh(*gpu.device, engine, 0);
    CHECK(irradiance(morning, {1.0f, 0.0f, 0.0f}).g > irradiance(morning, {-1.0f, 0.0f, 0.0f}).g * 1.08f);

    // A reflection probe in an empty scene sees the same sky: same SH, same lean.
    const entt::entity probe = world.create("Probe");
    world.registry().emplace<scene::ReflectionProbe>(probe, scene::ReflectionProbe{50.0f, 64});
    world.set_local(probe, glm::vec3(0.0f, 2.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    render_at(7.0f);
    CHECK(engine.baked_probes() == 1u);
    CHECK(engine.pending_probes() == 0u);
    const Sh captured = read_sh(*gpu.device, engine, 1);
    for (const glm::vec3 n :
         {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)}) {
        CHECK(irradiance(captured, n).g == doctest::Approx(irradiance(morning, n).g).epsilon(0.15));
    }
    // Moving it re-captures; so does a rebake request.
    world.set_local(probe, glm::vec3(1.0f, 2.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    world.update_transforms();
    engine.rebake_probes();
    render_at(7.0f);
    CHECK(engine.baked_probes() == 1u);
}

void shadow_checks(rhi::Backend backend) {
    Gpu gpu = make_gpu(backend);
    if (!gpu.device) {
        return;
    }
    RasterEngine engine(*gpu.device, *gpu.shaders);
    engine.grid().visible = false;
    engine.set_debug_lines(false);
    engine.lighting().sun_direction = glm::vec3(0.0f, 1.0f, 0.0f); // straight down
    engine.lighting().sun_color = glm::vec3(3.0f);

    scene::World world;
    assets::MeshLibrary meshes;
    assets::MeshData plane;
    plane.vertices = {{glm::vec3(-10, 0, -10), glm::vec3(0, 1, 0), 0xFFFFFFFFu},
                      {glm::vec3(-10, 0, 10), glm::vec3(0, 1, 0), 0xFFFFFFFFu},
                      {glm::vec3(10, 0, 10), glm::vec3(0, 1, 0), 0xFFFFFFFFu},
                      {glm::vec3(10, 0, -10), glm::vec3(0, 1, 0), 0xFFFFFFFFu}};
    plane.indices = {0, 1, 2, 0, 2, 3};
    meshes.add("plane", plane);
    meshes.add("sphere", assets::make_uv_sphere(1.0f, 32, 16));
    const entt::entity ground = world.create("Ground");
    world.registry().emplace<scene::MeshRenderer>(ground, scene::MeshRenderer{{core::Guid{}, "plane"}, true});
    const entt::entity ball = world.create("Ball");
    world.registry().emplace<scene::MeshRenderer>(ball, scene::MeshRenderer{{core::Guid{}, "sphere"}, true});
    world.set_local(ball, glm::vec3(3.0f, 2.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

    const render::View view = make_view({0.0f, 12.0f, 6.0f}, {0.0f, 0.0f, 0.0f});
    engine.render(gpu.device->context(), view, {&world, &meshes}, {gpu.output, {}});
    REQUIRE(engine.frame_lighting().shadow_light == 0);
    const core::Image image = gpu.device->context().read_texture(gpu.output);
    REQUIRE(image.width == 320);
    const auto pixel = [&](glm::vec3 p) {
        const glm::vec4 clip = view.view_projection() * glm::vec4(p, 1.0f);
        const auto x = static_cast<std::uint32_t>((clip.x / clip.w * 0.5f + 0.5f) * 320.0f);
        const auto y = static_cast<std::uint32_t>((0.5f - clip.y / clip.w * 0.5f) * 180.0f);
        return image.rgba[(static_cast<std::size_t>(y) * 320 + x) * 4];
    };
    // The ground straight under the ball is in its shadow; the mirrored spot isn't.
    const auto shadowed = pixel({3.0f, 0.0f, 0.0f});
    const auto lit = pixel({-3.0f, 0.0f, 0.0f});
    CHECK(static_cast<int>(lit) > static_cast<int>(shadowed) + 60);

    // Turning shadows off removes it.
    engine.shadow_settings().enabled = false;
    engine.render(gpu.device->context(), view, {&world, &meshes}, {gpu.output, {}});
    CHECK(engine.frame_lighting().shadow_light == -1);
    const core::Image unshadowed = gpu.device->context().read_texture(gpu.output);
    const auto at = [&](const core::Image& img, glm::vec3 p) {
        const glm::vec4 clip = view.view_projection() * glm::vec4(p, 1.0f);
        const auto x = static_cast<std::uint32_t>((clip.x / clip.w * 0.5f + 0.5f) * 320.0f);
        const auto y = static_cast<std::uint32_t>((0.5f - clip.y / clip.w * 0.5f) * 180.0f);
        return static_cast<int>(img.rgba[(static_cast<std::size_t>(y) * 320 + x) * 4]);
    };
    CHECK(std::abs(at(unshadowed, {3.0f, 0.0f, 0.0f}) - at(unshadowed, {-3.0f, 0.0f, 0.0f})) <= 2);
}

void hemisphere_checks(rhi::Backend backend) {
    Gpu gpu = make_gpu(backend);
    if (!gpu.device) {
        return;
    }
    RasterEngine engine(*gpu.device, *gpu.shaders);
    // A uniform environment of radiance L gives irradiance pi * L in every direction.
    engine.lighting().sky_color = glm::vec3(0.5f);
    engine.lighting().ground_color = glm::vec3(0.5f);
    engine.lighting().ambient = 1.0f;
    scene::World world;
    assets::MeshLibrary meshes;
    engine.render(gpu.device->context(), make_view({0, 2, 5}, {0, 0, 0}), {&world, &meshes}, {gpu.output, {}});
    CHECK_FALSE(engine.sky().physical());
    CHECK(engine.frame_lighting().exposure == 1.0f);
    const Sh sh = read_sh(*gpu.device, engine, 0);
    for (const glm::vec3 n : {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1)}) {
        CHECK(irradiance(sh, n).g == doctest::Approx(0.5f * 3.14159265f).epsilon(0.03));
    }
    // The prefiltered cube's sharpest mip reproduces the environment.
    const auto mip0 = gpu.device->context().read_texture_float(engine.sky().environment(), 0, 2);
    REQUIRE_FALSE(mip0.empty());
    CHECK(mip0[0] / kEnvScale == doctest::Approx(0.5f).epsilon(0.02));
}

} // namespace

TEST_CASE("sky: LUTs, environment and reflection probes (D3D12)") {
    sky_checks(rhi::Backend::D3D12);
}

TEST_CASE("sky: LUTs, environment and reflection probes (Vulkan)") {
    sky_checks(rhi::Backend::Vulkan);
}

TEST_CASE("sky: cascaded shadows darken what is under an occluder (D3D12, Vulkan)") {
    shadow_checks(rhi::Backend::D3D12);
    shadow_checks(rhi::Backend::Vulkan);
}

TEST_CASE("sky: without a Sky the environment is the hemisphere colours (D3D12, Vulkan)") {
    hemisphere_checks(rhi::Backend::D3D12);
    hemisphere_checks(rhi::Backend::Vulkan);
}
