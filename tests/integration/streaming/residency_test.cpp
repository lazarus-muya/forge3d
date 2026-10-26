// SPDX-License-Identifier: Apache-2.0
// GPU residency (phases.md P3.5): a 200-asset stress set streams under a 512 MB
// cap, levels follow screen size, idle assets are evicted, re-imports hot-swap.
#include <doctest/doctest.h>

#include "f3d/assets/cook.hpp"
#include "f3d/assets/mesh.hpp"
#include "f3d/assets/texture.hpp"
#include "f3d/render/raster/residency.hpp"
#include "f3d/render/rhi/device.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace rhi = f3d::render::rhi;
using f3d::render::raster::ResidencyManager;
using namespace f3d;

namespace {

/// A 1024^2 RGBA8 texture with a full mip chain (~5.6 MB on the GPU).
std::shared_ptr<const assets::CookedTexture> big_texture(std::uint8_t shade) {
    auto texture = std::make_shared<assets::CookedTexture>();
    texture->format = assets::TextureFormat::RGBA8;
    texture->srgb = true;
    for (std::uint32_t size = 1024;; size /= 2) {
        assets::TextureMip mip;
        mip.width = mip.height = size;
        mip.data.assign(static_cast<std::size_t>(size) * size * 4, shade);
        texture->mips.push_back(std::move(mip));
        if (size == 1) {
            break;
        }
    }
    return texture;
}

std::unique_ptr<rhi::Device> make_device() {
    rhi::DeviceDesc desc;
    desc.backend = rhi::default_backend();
    auto created = rhi::Device::create(desc);
    if (!created.device) {
        MESSAGE("skipping: ", created.error);
    }
    return std::move(created.device);
}

} // namespace

TEST_CASE("residency: 200 assets stay under a 512 MB cap, levels follow screen size") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const auto texture = big_texture(200);
    int loads = 0;
    render::raster::ResidencySources sources;
    sources.texture = [&](const core::AssetRef&) {
        ++loads;
        return texture;
    };
    ResidencyManager residency(*device, sources);
    constexpr std::uint64_t kCap = 512ull << 20;
    residency.set_budget(kCap);
    residency.set_idle_frames(10);

    std::vector<core::AssetRef> assets(200);
    for (std::size_t i = 0; i < assets.size(); ++i) {
        assets[i] = {core::Guid::generate(), "stress/texture_" + std::to_string(i) + ".png"};
    }
    // Everything wants mip 0 (a 1024 px texture over 512 px): 200 x 5.6 MB = 1.1 GB > cap.
    const auto frame = [&](std::size_t count, float near_px) {
        for (std::size_t i = 0; i < count; ++i) {
            // All above 512 px (all want mip 0), slightly smaller with i (lower priority).
            (void)residency.texture(assets[i], near_px / (1.0f + static_cast<float>(i) * 0.001f));
        }
        residency.end_frame();
        CHECK(residency.stats().used <= kCap); // after every frame
    };
    for (int f = 0; f < 30; ++f) {
        frame(assets.size(), 600.0f);
    }
    const auto& stats = residency.stats();
    MESSAGE("resident: ", stats.textures, " textures, ", stats.used >> 20, " MB of ", kCap >> 20, " MB; peak ",
            stats.peak >> 20, " MB; uploads ", stats.uploads, ", evictions ", stats.evictions);
    CHECK(stats.peak <= kCap);
    CHECK(stats.textures == 200); // every asset resident at some level
    CHECK(stats.used > kCap / 2); // the budget is actually used

    // The largest on screen get the finest levels, the smallest the coarsest.
    const auto rows = residency.rows();
    int nearest = -1;
    int farthest = -1;
    for (const auto& row : rows) {
        if (row.name == assets.front().path) {
            nearest = row.resident;
        }
        if (row.name == assets.back().path) {
            farthest = row.resident;
        }
    }
    CHECK(nearest == 0);
    CHECK(farthest > nearest);

    // Assets no longer drawn are evicted after idle_frames, least recently used first.
    for (int f = 0; f < 12; ++f) {
        frame(50, 600.0f);
    }
    CHECK(residency.stats().textures == 50);
    CHECK(residency.stats().evictions >= 150);
    // With room to spare, the remaining 50 all reach mip 0.
    for (const auto& row : residency.rows()) {
        if (row.resident >= 0) {
            CHECK(row.resident == 0);
        }
    }

    // A re-import (invalidate) swaps in the new data on the next frames.
    const auto* before = residency.texture(assets[0], 512.0f);
    REQUIRE(before != nullptr);
    void* old_native = before->texture.native();
    const int loads_before = loads;
    residency.end_frame();
    residency.invalidate(assets[0].guid);
    CHECK(residency.texture(assets[0], 512.0f) == nullptr); // gone until reloaded
    residency.end_frame();
    const auto* after = residency.texture(assets[0], 512.0f);
    REQUIRE(after != nullptr);
    CHECK(after->texture.native() != old_native);
    CHECK(loads > loads_before); // read from the source again
}

TEST_CASE("residency: meshes stream their LOD by screen size") {
    auto device = make_device();
    if (!device) {
        return;
    }
    const auto model =
        assets::load_model(std::filesystem::path(F3D_TEST_DATA_DIR) / "import" / "models" / "height_field.obj");
    REQUIRE(model.scene.has_value());
    const auto cooked = std::make_shared<assets::CookedMesh>(assets::cook_mesh(model.scene->meshes[0], {}));
    REQUIRE(cooked->lods.size() >= 3);
    render::raster::ResidencySources sources;
    sources.mesh = [&](const core::AssetRef&) { return std::shared_ptr<const assets::CookedMesh>(cooked); };
    ResidencyManager residency(*device, sources);
    const core::AssetRef ref{core::Guid::generate(), "project://height_field.obj#mesh/0"};

    CHECK(residency.mesh(ref, 400.0f) == nullptr); // first request: loaded at end_frame
    residency.end_frame();
    const auto* near = residency.mesh(ref, 400.0f);
    REQUIRE(near != nullptr);
    CHECK(near->lod == 0);
    CHECK(near->index_count == cooked->lods[0].index_count);
    const std::uint64_t near_bytes = residency.stats().used;
    residency.end_frame();

    (void)residency.mesh(ref, 20.0f); // far away: a coarse LOD, fewer bytes
    residency.end_frame();
    const auto* far = residency.mesh(ref, 20.0f);
    REQUIRE(far != nullptr);
    CHECK(far->lod > 0);
    CHECK(residency.stats().used < near_bytes);
    CHECK(residency.mesh_bounds(ref).extent().x == doctest::Approx(20.0f));
}
