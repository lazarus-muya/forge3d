// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/thumbnail_cache.hpp"

#include "f3d/assets/asset_index.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/render/raster/thumbnail_renderer.hpp"

#include <chrono>
#include <thread>

namespace f3d::editor {
namespace {

std::string thumbnail_path(const core::Guid& guid) {
    return "project://.forge3d/thumbnails/" + guid.to_string() + ".png";
}

} // namespace

ThumbnailCache::ThumbnailCache(render::rhi::Device& device, render::raster::ThumbnailRenderer* renderer,
                               core::vfs::FileSystem& fs, core::jobs::JobSystem& jobs)
    : device_(device), renderer_(renderer), fs_(fs), jobs_(jobs) {}

ThumbnailCache::~ThumbnailCache() {
    token_.cancel();
}

void ThumbnailCache::clear() {
    token_.cancel();
    token_ = core::jobs::CancellationToken{};
    entries_.clear();
    model_queue_.clear();
    in_flight_ = 0;
}

std::size_t ThumbnailCache::ready() const noexcept {
    std::size_t n = 0;
    for (const auto& [guid, entry] : entries_) {
        n += entry.texture ? 1 : 0;
    }
    return n;
}

const render::rhi::Texture& ThumbnailCache::get(const assets::AssetRecord& asset) {
    if (asset.stale()) {
        return empty_; // nothing cooked to show yet
    }
    Entry& entry = entries_[asset.guid];
    if (entry.hash != asset.imported_hash) {
        entry = {}; // re-imported: the old thumbnail is out of date
    }
    if (!entry.requested) {
        entry.requested = true;
        entry.hash = asset.imported_hash;
        request(asset);
    }
    return entry.texture ? entry.texture : empty_;
}

void ThumbnailCache::request(const assets::AssetRecord& asset) {
    // 1. A cached PNG made from the same import.
    if (index_) {
        if (const auto cached = index_->thumbnail(asset.guid); cached && cached->source_hash == asset.imported_hash) {
            ++in_flight_;
            const std::string path = cached->path;
            jobs_.submit(
                core::jobs::Priority::Interactive, token_,
                [this, path](const core::jobs::CancellationToken&) {
                    const auto bytes = fs_.read(path);
                    return bytes ? core::decode_image(*bytes) : std::nullopt;
                },
                [this, asset](std::optional<core::Image> image) {
                    --in_flight_;
                    if (image) {
                        deliver(asset.guid, asset.imported_hash, *image, false);
                    } else {
                        // Missing or damaged: make it again.
                        if (asset.type() == "model") {
                            model_queue_.push_back(asset);
                        } else {
                            entries_[asset.guid].requested = false;
                        }
                    }
                });
            return;
        }
    }
    // 2. Make it: textures on a worker, models on the main thread (GPU).
    if (asset.type() == "model") {
        model_queue_.push_back(asset);
        return;
    }
    ++in_flight_;
    jobs_.submit(
        core::jobs::Priority::Interactive, token_,
        [this, guid = asset.guid](const core::jobs::CancellationToken&) -> std::optional<core::Image> {
            const auto texture = assets::load_cooked_texture(fs_, guid);
            if (!texture) {
                return std::nullopt;
            }
            return assets::texture_thumbnail(*texture, kSize);
        },
        [this, asset](std::optional<core::Image> image) {
            --in_flight_;
            if (image) {
                deliver(asset.guid, asset.imported_hash, *image, true);
            }
        });
}

render::rhi::Texture ThumbnailCache::upload(const core::Image& image, const core::Guid& guid) {
    render::rhi::TextureDesc desc;
    desc.name = "Thumbnail " + guid.to_string();
    desc.width = image.width;
    desc.height = image.height;
    desc.format = render::rhi::Format::RGBA8Unorm; // sRGB-encoded values, shown as is (like the viewport image)
    desc.bind = render::rhi::BindFlags::ShaderResource;
    return device_.create_texture(desc, image.rgba.data(), static_cast<std::size_t>(image.width) * 4);
}

void ThumbnailCache::deliver(const core::Guid& guid, const std::string& hash, const core::Image& image, bool store) {
    const auto it = entries_.find(guid);
    if (it == entries_.end() || it->second.hash != hash || image.empty()) {
        return; // dropped or re-imported meanwhile
    }
    it->second.texture = upload(image, guid);
    if (store) {
        const std::string path = thumbnail_path(guid);
        if (fs_.write(path, core::encode_png(image)) && index_) {
            index_->set_thumbnail(guid, {path, hash});
        }
    }
}

void ThumbnailCache::update(std::size_t model_budget) {
    for (std::size_t i = 0; i < model_budget && !model_queue_.empty(); ++i) {
        const assets::AssetRecord asset = model_queue_.front();
        model_queue_.pop_front();
        if (!renderer_) {
            continue;
        }
        F3D_ZONE_N("Model thumbnail");
        std::string error;
        const auto model = assets::load_cooked_model(fs_, asset.guid, &error);
        if (!model) {
            F3D_LOG_DEBUG("No thumbnail for {}: {}", asset.path, error);
            continue;
        }
        assets::ModelScene scene;
        scene.name = model->name;
        scene.nodes = model->nodes;
        for (const assets::CookedMesh& mesh : model->meshes) {
            scene.meshes.push_back(mesh.to_mesh_data(0));
        }
        deliver(asset.guid, asset.imported_hash, renderer_->render(scene, kSize), true);
    }
}

void ThumbnailCache::finish_all() {
    // Bounded: a thumbnail that never arrives must not freeze the editor.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (pending() > 0 && std::chrono::steady_clock::now() < deadline) {
        update(model_queue_.size());
        jobs_.run_continuations();
        if (in_flight_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (pending() > 0) {
        F3D_LOG_WARN("Thumbnails: {} still pending after 60 s", pending());
    }
}

} // namespace f3d::editor
