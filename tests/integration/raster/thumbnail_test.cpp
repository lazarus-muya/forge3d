// SPDX-License-Identifier: Apache-2.0
// Offscreen model thumbnails (phases.md P3.3): the model fills the frame, the
// corners show the background.
#include <doctest/doctest.h>

#include "f3d/assets/mesh.hpp"
#include "f3d/core/image.hpp"
#include "f3d/render/raster/thumbnail_renderer.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"

#include <filesystem>

namespace rhi = f3d::render::rhi;

namespace {

void thumbnail_check(rhi::Backend backend) {
    rhi::DeviceDesc desc;
    desc.backend = backend;
    desc.allow_fallback = false;
    auto created = rhi::Device::create(desc);
    if (!created.device) {
        MESSAGE("skipping ", rhi::to_string(backend), ": ", created.error);
        return;
    }
    rhi::ShaderLibrary shaders(*created.device, rhi::default_shader_library_desc());
    f3d::render::raster::ThumbnailRenderer renderer(*created.device, shaders);

    const auto fox = f3d::assets::load_model(std::filesystem::path(F3D_TEST_DATA_DIR) / "gltf" / "Fox.glb");
    REQUIRE(fox.scene.has_value());
    const f3d::core::Image image = renderer.render(*fox.scene, 128);
    INFO("backend ", rhi::to_string(backend));
    REQUIRE(image.width == 128);
    REQUIRE(image.height == 128);
    f3d::core::write_png(std::filesystem::path(F3D_TEST_OUTPUT_DIR) /
                             ("thumbnail-fox-" + std::string(rhi::to_string(backend)) + ".png"),
                         image);

    const auto background = image.pixel(0, 0);
    CHECK(image.pixel(127, 0) == background);
    CHECK(image.pixel(0, 127) == background);
    CHECK(image.pixel(127, 127) == background);
    std::size_t covered = 0;
    std::uint32_t min_x = 128, max_x = 0, min_y = 128, max_y = 0;
    for (std::uint32_t y = 0; y < 128; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            if (image.pixel(x, y) != background) {
                ++covered;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }
    CHECK(covered > 128 * 128 / 10); // the model is visible...
    CHECK(min_x > 0);                // ...and framed: not cut off at the edges
    CHECK(max_x < 127);
    CHECK((max_x - min_x) > 64); // ...and fills a good part of the frame
    CHECK((min_x + max_x) / 2 > 40);
    CHECK((min_x + max_x) / 2 < 88); // roughly centred
    (void)min_y;
    (void)max_y;
}

} // namespace

TEST_CASE("raster: model thumbnails are framed and lit (D3D12)") {
    if (rhi::is_backend_available(rhi::Backend::D3D12)) {
        thumbnail_check(rhi::Backend::D3D12);
    }
}

TEST_CASE("raster: model thumbnails are framed and lit (Vulkan)") {
    if (rhi::is_backend_available(rhi::Backend::Vulkan)) {
        thumbnail_check(rhi::Backend::Vulkan);
    }
}
