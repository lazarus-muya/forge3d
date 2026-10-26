// SPDX-License-Identifier: Apache-2.0
// Draws a triangle offscreen on every available backend with the build's
// precompiled (DXC) shaders and checks pixels: vertex/uniform buffers, input
// layout, winding/culling and the Y-up NDC convention must agree across backends.
#include <doctest/doctest.h>

#include "f3d/core/image.hpp"
#include "f3d/core/log.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"

#include <array>
#include <cstdlib>
#include <string>
#include <utility>

namespace rhi = f3d::render::rhi;

namespace {

struct Vertex {
    float x, y;
    float r, g, b;
};

void draw_and_check(rhi::Backend backend) {
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

    rhi::GraphicsPipelineDesc pd;
    pd.name = "test triangle";
    pd.vs = shaders.load("test_triangle.vs");
    pd.ps = shaders.load("test_triangle.ps");
    REQUIRE(pd.vs);
    REQUIRE(pd.ps);
    pd.vertex_layout = {{0, 0, rhi::Format::RG32Float, 0}, {1, 0, rhi::Format::RGB32Float, 8}};
    pd.color_formats = {rhi::Format::RGBA8Unorm};
    pd.cull = rhi::CullMode::Back; // the triangle is counter-clockwise: culling a CW triangle would fail the test
    const rhi::Pipeline pipeline = device.create_graphics_pipeline(pd);
    REQUIRE(pipeline);

    // Upper-left half of the screen (NDC y up), counter-clockwise.
    const std::array<Vertex, 3> vertices{
        {{-1.f, 1.f, 1.f, 0.5f, 0.25f}, {-1.f, -1.f, 1.f, 0.5f, 0.25f}, {1.f, 1.f, 1.f, 0.5f, 0.25f}}};
    const rhi::Buffer vb = device.create_buffer(
        {"triangle vb", sizeof(vertices), rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable}, vertices.data());
    const rhi::Buffer cb =
        device.create_buffer({"triangle cb", 16, rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    const rhi::Texture target = device.create_texture({"triangle rt", 64, 64, rhi::Format::RGBA8Unorm,
                                                       rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
    REQUIRE(vb);
    REQUIRE(cb);
    REQUIRE(target);

    rhi::Bindings bindings = pipeline.create_bindings();
    REQUIRE(bindings);
    CHECK(bindings.set("Constants", cb));

    rhi::CommandContext& ctx = device.context();
    const std::array<float, 4> tint{1.0f, 1.0f, 2.0f, 1.0f}; // blue doubled: 0.25 -> 0.5
    ctx.update_buffer(cb, tint.data(), sizeof(tint));
    ctx.set_render_target(target);
    ctx.clear_color(target, {0.0f, 0.0f, 0.0f, 1.0f});
    ctx.set_pipeline(pipeline);
    ctx.commit(bindings);
    ctx.set_vertex_buffer(vb);
    ctx.draw(3);

    const f3d::core::Image image = ctx.read_texture(target);
    REQUIRE(image.width == 64);
    REQUIRE(image.height == 64);
    // The triangle covers u + v < 1 (u right, v down). A vertical flip would cover u < v instead,
    // and a winding mismatch would cull it entirely.
    INFO("backend ", rhi::to_string(backend));
    for (const auto [x, y] : {std::pair{12u, 6u}, std::pair{56u, 4u}, std::pair{4u, 52u}}) {
        const auto p = image.pixel(x, y);
        INFO("covered pixel ", x, ",", y);
        CHECK(int(p[0]) == 255);
        CHECK(std::abs(int(p[1]) - 128) <= 1); // 0.5 green
        CHECK(std::abs(int(p[2]) - 128) <= 1); // 0.25 blue * tint 2
    }
    for (const auto [x, y] : {std::pair{52u, 52u}, std::pair{60u, 60u}}) {
        const auto p = image.pixel(x, y);
        INFO("clear pixel ", x, ",", y);
        CHECK(int(p[0]) == 0);
        CHECK(int(p[1]) == 0);
        CHECK(int(p[2]) == 0);
    }

    const auto out =
        std::string(F3D_TEST_OUTPUT_DIR) + "/rhi-triangle-" + std::string(rhi::to_string(backend)) + ".png";
    f3d::core::write_png(out, image);
}

} // namespace

TEST_CASE("rhi: triangle renders identically on D3D12") {
    if (!rhi::is_backend_available(rhi::Backend::D3D12)) {
        return;
    }
    draw_and_check(rhi::Backend::D3D12);
}

TEST_CASE("rhi: triangle renders identically on Vulkan") {
    if (!rhi::is_backend_available(rhi::Backend::Vulkan)) {
        return;
    }
    draw_and_check(rhi::Backend::Vulkan);
}
