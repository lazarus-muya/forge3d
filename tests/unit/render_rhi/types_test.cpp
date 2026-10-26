// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/render/rhi/types.hpp"

namespace rhi = f3d::render::rhi;

TEST_CASE("rhi: backend names") {
    CHECK(rhi::parse_backend("d3d12") == rhi::Backend::D3D12);
    CHECK(rhi::parse_backend("vulkan") == rhi::Backend::Vulkan);
    CHECK(rhi::parse_backend("vk") == rhi::Backend::Vulkan);
    CHECK_FALSE(rhi::parse_backend("metal").has_value());
    CHECK(rhi::to_string(rhi::Backend::D3D12) == "d3d12");
    CHECK(rhi::is_backend_available(rhi::default_backend()));
}

TEST_CASE("rhi: format properties") {
    CHECK(rhi::format_size(rhi::Format::RGBA8UnormSrgb) == 4);
    CHECK(rhi::format_size(rhi::Format::RGB32Float) == 12);
    CHECK(rhi::format_size(rhi::Format::RGBA32Float) == 16);
    CHECK(rhi::is_depth_format(rhi::Format::D32Float));
    CHECK_FALSE(rhi::is_depth_format(rhi::Format::R32Float));
    CHECK(rhi::is_srgb_format(rhi::Format::BGRA8UnormSrgb));
    CHECK(rhi::has_flag(rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource, rhi::BindFlags::ShaderResource));
    CHECK_FALSE(rhi::has_flag(rhi::BindFlags::RenderTarget, rhi::BindFlags::DepthStencil));
}

TEST_CASE("rhi: shader stage from name") {
    CHECK(rhi::stage_from_name("grid.vs") == rhi::ShaderStage::Vertex);
    CHECK(rhi::stage_from_name("grid.ps") == rhi::ShaderStage::Pixel);
    CHECK(rhi::stage_from_name("blur.cs") == rhi::ShaderStage::Compute);
    CHECK_FALSE(rhi::stage_from_name("grid").has_value());
}

TEST_CASE("rhi: default shader library points at the build's shader directory") {
    const auto desc = rhi::default_shader_library_desc();
    CHECK_FALSE(desc.binary_dir.empty());
}
