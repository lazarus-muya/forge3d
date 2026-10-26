// SPDX-License-Identifier: Apache-2.0
#pragma once

// Asset thumbnails for the browser (phases.md P3.3). A thumbnail is a PNG in
// "project://.forge3d/thumbnails/<guid>.png", recorded in the asset index with
// the source hash it was made from, so it is reused until the asset is
// re-imported. Textures are thumbnailed on workers (decoding a cooked mip);
// models are rendered offscreen on the main thread, a few per frame.

#include "f3d/assets/asset_database.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/image.hpp"
#include "f3d/core/jobs.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/render/rhi/device.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

namespace f3d::assets {
class AssetIndex;
} // namespace f3d::assets

namespace f3d::render::raster {
class ThumbnailRenderer;
} // namespace f3d::render::raster

namespace f3d::editor {

class ThumbnailCache {
public:
    static constexpr std::uint32_t kSize = 128;

    ThumbnailCache(render::rhi::Device& device, render::raster::ThumbnailRenderer* renderer, core::vfs::FileSystem& fs,
                   core::jobs::JobSystem& jobs);
    ~ThumbnailCache();

    /// The thumbnail texture of `asset`, or an empty texture while it is being
    /// made (or if the asset isn't imported). Requests it on first use.
    [[nodiscard]] const render::rhi::Texture& get(const assets::AssetRecord& asset);

    /// Main thread, once per frame: renders up to `model_budget` model thumbnails
    /// and turns finished images into textures.
    void update(std::size_t model_budget = 2);
    /// Makes every requested thumbnail now (blocking; automated captures).
    void finish_all();

    /// Where thumbnails are recorded (null: memory only).
    void set_index(assets::AssetIndex* index) noexcept { index_ = index; }
    /// Drops every cached thumbnail (project changed).
    void clear();

    [[nodiscard]] std::size_t ready() const noexcept;
    [[nodiscard]] std::size_t pending() const noexcept { return model_queue_.size() + in_flight_; }

private:
    struct Entry {
        std::string hash; ///< imported source hash the thumbnail shows
        render::rhi::Texture texture;
        bool requested = false;
    };
    void request(const assets::AssetRecord& asset);
    void deliver(const core::Guid& guid, const std::string& hash, const core::Image& image, bool store);
    render::rhi::Texture upload(const core::Image& image, const core::Guid& guid);

    render::rhi::Device& device_;
    render::raster::ThumbnailRenderer* renderer_;
    core::vfs::FileSystem& fs_;
    core::jobs::JobSystem& jobs_;
    core::jobs::CancellationToken token_;
    assets::AssetIndex* index_ = nullptr;
    std::unordered_map<core::Guid, Entry> entries_;
    std::deque<assets::AssetRecord> model_queue_;
    std::size_t in_flight_ = 0;
    render::rhi::Texture empty_;
};

} // namespace f3d::editor
