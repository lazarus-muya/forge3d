// SPDX-License-Identifier: Apache-2.0
// Automatic and manual exposure on the GPU (phases.md P4.4).
#include <doctest/doctest.h>

#include "f3d/assets/mesh_library.hpp"
#include "f3d/render/raster/atmosphere.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <cmath>
#include <memory>

using namespace f3d;
namespace rhi = f3d::render::rhi;
using namespace f3d::render::raster;

namespace {

void exposure_checks(rhi::Backend backend) {
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
    engine.set_frame_delta(0.0f); // no adaptation unless a test asks for it
    const rhi::Texture output = device.create_texture({"Output", 64, 64, RasterEngine::display_format(),
                                                       rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});

    scene::World world;
    assets::MeshLibrary meshes;
    const entt::entity sky = world.create("Sky");
    world.registry().emplace<scene::Sky>(sky);

    // A narrow view straight up: every pixel is the zenith sky.
    render::View view;
    view.camera_position = glm::vec3(0.0f, 2.0f, 0.0f);
    view.view = glm::lookAtRH(view.camera_position, view.camera_position + glm::vec3(0.0f, 1.0f, 0.0f),
                              glm::vec3(0.0f, 0.0f, -1.0f));
    view.width = view.height = 64;
    view.near_plane = 0.1f;
    view.far_plane = 100.0f;
    view.projection = glm::perspectiveRH_ZO(glm::radians(2.0f), 1.0f, view.near_plane, view.far_plane);
    const auto frame = [&] { engine.render(device.context(), view, {&world, &meshes}, {output, {}}); };

    // Automatic: after one metered frame the exposure is the reflected-light reading of
    // the zenith (the sky-view LUT's luminance there), within a histogram bin or two.
    engine.settings().exposure.low_percent = 0.0f;
    engine.settings().exposure.high_percent = 1.0f;
    engine.reset_exposure();
    frame();
    frame();
    const auto lut = device.context().read_texture_float(engine.sky().sky_view_lut());
    const std::size_t top =
        (static_cast<std::size_t>(engine.sky().sky_view_lut().height() - 1) * engine.sky().sky_view_lut().width() +
         engine.sky().sky_view_lut().width() / 2) *
        4;
    const float zenith = (lut[top] * 0.2126f + lut[top + 1] * 0.7152f + lut[top + 2] * 0.0722f) / kEnvScale;
    const glm::vec4 metered = engine.read_exposure();
    CHECK(metered.z == 1.0f);
    CHECK(metered.y == doctest::Approx(ev100_from_luminance(zenith)).epsilon(0.03));
    CHECK(metered.x == doctest::Approx(exposure_from_ev100(metered.y)).epsilon(1e-4));

    // Without time passing nothing adapts, even when the scene changes.
    world.registry().get<scene::Sky>(sky).time_of_day = 19.5f;
    frame();
    frame();
    CHECK(engine.read_exposure().y == doctest::Approx(metered.y).epsilon(1e-4));
    // With time it moves towards the darker scene, part of the way per frame.
    engine.set_frame_delta(0.1f);
    frame();
    const float step = engine.read_exposure().y;
    CHECK(step < metered.y - 0.05f);
    engine.reset_exposure();
    frame();
    frame();
    const float settled = engine.read_exposure().y;
    CHECK(settled < step);

    // Compensation: +1 EV brighter is one EV100 lower.
    engine.settings().exposure.compensation = 1.0f;
    engine.reset_exposure();
    frame();
    frame();
    CHECK(engine.read_exposure().y == doctest::Approx(settled - 1.0f).epsilon(0.02));

    // Manual: exactly the EV100 asked for (minus compensation).
    engine.settings().exposure.mode = ExposureMode::Manual;
    engine.settings().exposure.manual_ev100 = 11.0f;
    engine.settings().exposure.compensation = 0.0f;
    frame();
    frame();
    CHECK(engine.read_exposure().y == doctest::Approx(11.0f));
    CHECK(engine.frame_lighting().gpu_exposure);

    // Without a Sky: relative units, no metering, exposure 2^compensation.
    world.registry().remove<scene::Sky>(sky);
    engine.settings().exposure.compensation = 1.0f;
    frame();
    CHECK_FALSE(engine.frame_lighting().gpu_exposure);
    CHECK(engine.frame_lighting().exposure_scale == doctest::Approx(2.0f));
}

} // namespace

TEST_CASE("exposure: histogram metering, adaptation, compensation and manual EV (D3D12, Vulkan)") {
    exposure_checks(rhi::Backend::D3D12);
    exposure_checks(rhi::Backend::Vulkan);
}
