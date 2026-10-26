// SPDX-License-Identifier: Apache-2.0
// Auto-reimport (phases.md P3.5): touching a source texture is noticed by the
// source watcher, the rescan marks it stale, the re-import writes a new cooked
// texture and the resident GPU copy is swapped for it — the editor's loop
// (App::watch_asset_sources -> start_asset_scan -> ImportQueue -> asset_imported).
#include <doctest/doctest.h>

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/assets/source_watcher.hpp"
#include "f3d/core/image.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/render/raster/residency.hpp"
#include "f3d/render/rhi/device.hpp"

#include <chrono>
#include <filesystem>
#include <memory>

namespace rhi = f3d::render::rhi;
using namespace f3d;
namespace fs = std::filesystem;

namespace {

core::Image solid(std::uint32_t size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    core::Image image;
    image.width = image.height = size;
    image.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = r;
        image.rgba[i + 1] = g;
        image.rgba[i + 2] = b;
        image.rgba[i + 3] = 255;
    }
    return image;
}

} // namespace

TEST_CASE("auto-reimport: touching a source texture re-imports it and swaps the GPU copy") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::default_backend();
    auto created = rhi::Device::create(desc);
    if (!created.device) {
        MESSAGE("skipping: ", created.error);
        return;
    }
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "hot_reload_project";
    fs::remove_all(dir);
    fs::create_directories(dir / "textures");
    const fs::path source = dir / "textures" / "albedo.png";
    REQUIRE(core::write_png(source, solid(64, 200, 40, 40)));

    core::vfs::FileSystem files;
    files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
    assets::AssetDatabase db;
    db.rescan(files, "project://");
    REQUIRE(db.size() == 1);
    const assets::AssetRecord record = db.records().front();
    REQUIRE(record.type() == "texture");
    REQUIRE(assets::import_asset(files, record).ok);
    db.rescan(files, "project://");
    REQUIRE_FALSE(db.records().front().stale());

    // The texture is resident, as the viewport would have it.
    int loads = 0;
    render::raster::ResidencySources sources;
    sources.texture = [&](const core::AssetRef& texture_ref) -> std::shared_ptr<const assets::CookedTexture> {
        ++loads;
        auto texture = assets::load_cooked_texture(files, texture_ref);
        return texture ? std::make_shared<const assets::CookedTexture>(std::move(*texture)) : nullptr;
    };
    render::raster::ResidencyManager residency(*created.device, sources);
    const core::AssetRef ref{record.guid, record.path};
    (void)residency.texture(ref, 64.0f);
    residency.end_frame();
    const auto* before = residency.texture(ref, 64.0f);
    REQUIRE(before != nullptr);
    const rhi::Texture old_texture = before->texture; // kept alive: its address can't be reused
    void* old_native = old_texture.native();
    const auto old_cooked = assets::load_cooked_texture(files, record.guid);
    REQUIRE(old_cooked.has_value());

    assets::SourceWatcher watcher;
    watcher.sync(files, db);
    CHECK(watcher.watched() == 1);
    CHECK(watcher.poll().empty()); // the baseline

    // Edit the source outside the editor (a later stamp, as a paint program saving would).
    REQUIRE(core::write_png(source, solid(64, 30, 60, 220)));
    fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(2));
    const std::vector<core::Guid> changed = watcher.poll();
    REQUIRE(changed.size() == 1);
    CHECK(changed.front() == record.guid);
    CHECK(watcher.poll().empty()); // reported once

    // The rescan sees the new hash: stale, re-imported.
    db.rescan(files, "project://");
    const assets::AssetRecord* stale = db.find(record.guid);
    REQUIRE(stale != nullptr);
    REQUIRE(stale->stale());
    REQUIRE(assets::import_asset(files, *stale).ok);
    db.rescan(files, "project://");
    CHECK_FALSE(db.find(record.guid)->stale());
    const auto new_cooked = assets::load_cooked_texture(files, record.guid);
    REQUIRE(new_cooked.has_value());
    CHECK(new_cooked->mips.front().data != old_cooked->mips.front().data);

    // asset_imported -> invalidate: the next frames use the new texture.
    const int loads_before = loads;
    residency.invalidate(record.guid);
    (void)residency.texture(ref, 64.0f);
    residency.end_frame();
    const auto* after = residency.texture(ref, 64.0f);
    REQUIRE(after != nullptr);
    CHECK(after->texture.native() != old_native);
    CHECK(loads > loads_before); // read from the new cooked file

    // The watcher keeps its baseline across database changes (no spurious reports).
    watcher.sync(files, db);
    CHECK(watcher.poll().empty());
}
