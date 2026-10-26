// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/residency.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace f3d::render::raster {
namespace {

rhi::Format gpu_format(assets::TextureFormat format, bool srgb) {
    switch (format) {
    case assets::TextureFormat::RGBA8:
        return srgb ? rhi::Format::RGBA8UnormSrgb : rhi::Format::RGBA8Unorm;
    case assets::TextureFormat::BC1:
        return srgb ? rhi::Format::BC1UnormSrgb : rhi::Format::BC1Unorm;
    case assets::TextureFormat::BC3:
        return srgb ? rhi::Format::BC3UnormSrgb : rhi::Format::BC3Unorm;
    case assets::TextureFormat::BC4:
        return rhi::Format::BC4Unorm;
    case assets::TextureFormat::BC5:
        return rhi::Format::BC5Unorm;
    case assets::TextureFormat::BC7:
        return srgb ? rhi::Format::BC7UnormSrgb : rhi::Format::BC7Unorm;
    case assets::TextureFormat::RGBA16F:
        return rhi::Format::RGBA16Float;
    }
    return rhi::Format::Unknown;
}

std::size_t row_pitch(assets::TextureFormat format, std::uint32_t width) {
    if (const std::size_t block = assets::block_bytes(format)) {
        return static_cast<std::size_t>((width + 3) / 4) * block;
    }
    return static_cast<std::size_t>(width) * (format == assets::TextureFormat::RGBA16F ? 8 : 4);
}

/// Vertices used by a LOD, for its compacted size.
std::uint32_t used_vertices(const assets::CookedMesh& mesh, const assets::MeshLod& lod) {
    std::unordered_set<std::uint32_t> used;
    used.reserve(lod.index_count);
    for (std::uint32_t i = 0; i < lod.index_count; ++i) {
        used.insert(mesh.indices[lod.first_index + i]);
    }
    return static_cast<std::uint32_t>(used.size());
}

} // namespace

rhi::Texture upload_texture(rhi::Device& device, const assets::CookedTexture& cpu, std::uint32_t top_mip,
                            const std::string& name) {
    if (top_mip >= cpu.mips.size()) {
        return {};
    }
    std::vector<rhi::TextureLevelData> levels;
    for (std::size_t m = top_mip; m < cpu.mips.size(); ++m) {
        levels.push_back({cpu.mips[m].data.data(), row_pitch(cpu.format, cpu.mips[m].width)});
    }
    rhi::TextureDesc desc;
    desc.name = name;
    desc.width = cpu.mips[top_mip].width;
    desc.height = cpu.mips[top_mip].height;
    desc.format = gpu_format(cpu.format, cpu.srgb);
    desc.bind = rhi::BindFlags::ShaderResource;
    return device.create_texture(desc, levels);
}

ResidencyManager::ResidencyManager(rhi::Device& device, ResidencySources sources)
    : device_(device), sources_(std::move(sources)) {}

ResidencyManager::~ResidencyManager() = default;

std::string ResidencyManager::key(const core::AssetRef& ref, bool texture) {
    // GUID + sub-asset ("#mesh/2", "#image/0"): paths may move, sub-assets don't.
    const auto hash = ref.path.rfind('#');
    return (texture ? "t:" : "m:") + ref.guid.to_string() + (hash == std::string::npos ? "" : ref.path.substr(hash));
}

int ResidencyManager::lod_for(float screen_px, int lods) noexcept {
    if (lods <= 1 || screen_px >= 200.0f) {
        return 0;
    }
    const int lod = 1 + static_cast<int>(std::floor(std::log2(200.0f / std::max(screen_px, 1.0f))));
    return std::clamp(lod, 0, lods - 1);
}

int ResidencyManager::mip_for(float screen_px, std::uint32_t width, int mips) noexcept {
    const float texels = std::max(2.0f * screen_px, 1.0f); // texels needed across the object
    const int mip = static_cast<int>(std::floor(std::log2(static_cast<float>(width) / texels)));
    return std::clamp(mip, 0, std::max(mips - 1, 0));
}

ResidencyManager::Entry* ResidencyManager::entry(const core::AssetRef& ref, bool texture) {
    if (ref.guid.is_null()) {
        return nullptr;
    }
    const std::string k = key(ref, texture);
    if (const auto it = entries_.find(k); it != entries_.end()) {
        return it->second.levels > 0 ? &it->second : nullptr;
    }
    Entry e;
    e.ref = ref;
    e.is_texture = texture;
    if (texture) {
        // Only the level sizes are kept; the texels are read again when a level is loaded.
        const auto cpu = sources_.texture ? sources_.texture(ref) : nullptr;
        if (cpu && !cpu->mips.empty()) {
            e.levels = static_cast<int>(cpu->mips.size());
            e.width = cpu->mips[0].width;
            e.level_bytes.assign(cpu->mips.size(), 0);
            std::uint64_t sum = 0;
            for (std::size_t i = cpu->mips.size(); i-- > 0;) {
                sum += cpu->mips[i].data.size();
                e.level_bytes[i] = sum;
            }
        }
    } else {
        e.cpu_mesh = sources_.mesh ? sources_.mesh(ref) : nullptr;
        if (e.cpu_mesh && !e.cpu_mesh->lods.empty()) {
            e.levels = static_cast<int>(e.cpu_mesh->lods.size());
            for (const assets::MeshLod& lod : e.cpu_mesh->lods) {
                const std::uint64_t vertices = used_vertices(*e.cpu_mesh, lod);
                e.level_bytes.push_back(vertices * (sizeof(assets::MeshVertex) + sizeof(glm::vec2)) +
                                        static_cast<std::uint64_t>(lod.index_count) * sizeof(std::uint32_t));
            }
        }
    }
    if (e.levels == 0) {
        F3D_LOG_WARN("Residency: {} {} is not available", texture ? "texture" : "mesh", ref.path);
    }
    auto [it, inserted] = entries_.emplace(k, std::move(e));
    (void)inserted;
    return it->second.levels > 0 ? &it->second : nullptr;
}

const GpuMeshLod* ResidencyManager::mesh(const core::AssetRef& ref, float screen_px) {
    Entry* e = entry(ref, false);
    if (!e) {
        return nullptr;
    }
    e->last_used = frame_;
    e->screen_px = std::max(e->wanted < 0 ? 0.0f : e->screen_px, screen_px);
    e->wanted = lod_for(e->screen_px, e->levels);
    return e->resident >= 0 ? &e->mesh : nullptr;
}

const GpuTexture* ResidencyManager::texture(const core::AssetRef& ref, float screen_px) {
    Entry* e = entry(ref, true);
    if (!e) {
        return nullptr;
    }
    e->last_used = frame_;
    e->screen_px = std::max(e->wanted < 0 ? 0.0f : e->screen_px, screen_px);
    e->wanted = mip_for(e->screen_px, e->width, e->levels);
    return e->resident >= 0 ? &e->texture : nullptr;
}

core::Bounds ResidencyManager::mesh_bounds(const core::AssetRef& ref) {
    Entry* e = entry(ref, false);
    return e && e->cpu_mesh ? e->cpu_mesh->bounds : core::Bounds{};
}

bool ResidencyManager::load(Entry& e, int level) {
    F3D_ZONE;
    if (e.is_texture) {
        const auto cpu = sources_.texture ? sources_.texture(e.ref) : nullptr;
        if (!cpu || level >= static_cast<int>(cpu->mips.size())) {
            return false;
        }
        rhi::Texture texture =
            upload_texture(device_, *cpu, static_cast<std::uint32_t>(level), "Streamed " + e.ref.path);
        if (!texture) {
            return false;
        }
        e.texture = {std::move(texture), static_cast<std::uint32_t>(level)};
    } else {
        const assets::CookedMesh& cpu = *e.cpu_mesh;
        const assets::MeshLod& lod = cpu.lods[static_cast<std::size_t>(level)];
        // Compact the vertices this LOD uses.
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        std::vector<assets::MeshVertex> vertices;
        std::vector<glm::vec2> uvs;
        std::vector<std::uint32_t> indices(lod.index_count);
        for (std::uint32_t i = 0; i < lod.index_count; ++i) {
            const std::uint32_t old = cpu.indices[lod.first_index + i];
            auto [it, inserted] = remap.emplace(old, static_cast<std::uint32_t>(vertices.size()));
            if (inserted) {
                vertices.push_back(cpu.vertices[old]);
                uvs.push_back(cpu.uvs.empty() ? glm::vec2(0.0f) : cpu.uvs[old]);
            }
            indices[i] = it->second;
        }
        GpuMeshLod gpu;
        gpu.submeshes = cpu.lod_submeshes(static_cast<std::size_t>(level));
        gpu.vertices = device_.create_buffer({cpu.name + " LOD" + std::to_string(level) + " vertices",
                                              vertices.size() * sizeof(assets::MeshVertex),
                                              rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                                             vertices.data());
        gpu.uvs =
            device_.create_buffer({cpu.name + " LOD" + std::to_string(level) + " uvs", uvs.size() * sizeof(glm::vec2),
                                   rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                                  uvs.data());
        gpu.indices = device_.create_buffer({cpu.name + " LOD" + std::to_string(level) + " indices",
                                             indices.size() * sizeof(std::uint32_t), rhi::BindFlags::IndexBuffer,
                                             rhi::Usage::Immutable},
                                            indices.data());
        if (!gpu.vertices || !gpu.uvs || !gpu.indices) {
            return false;
        }
        gpu.index_count = lod.index_count;
        gpu.lod = static_cast<std::uint32_t>(level);
        e.mesh = std::move(gpu);
    }
    stats_.used -= e.resident_bytes;
    e.resident = level;
    e.resident_bytes = e.level_bytes[static_cast<std::size_t>(level)];
    stats_.used += e.resident_bytes;
    ++stats_.uploads;
    ++generation_;
    return true;
}

void ResidencyManager::unload(Entry& e) {
    if (e.resident < 0) {
        return;
    }
    e.mesh = {};
    e.texture = {};
    stats_.used -= e.resident_bytes;
    e.resident = -1;
    e.resident_bytes = 0;
    ++stats_.evictions;
    ++generation_;
}

void ResidencyManager::end_frame() {
    F3D_ZONE;
    const std::uint64_t now = frame_;
    std::vector<Entry*> requested;
    std::vector<Entry*> idle; // resident, not requested this frame
    for (auto& [k, e] : entries_) {
        if (e.levels == 0) {
            continue;
        }
        if (e.last_used == now) {
            requested.push_back(&e);
        } else if (e.resident >= 0) {
            if (now - e.last_used > idle_frames_) {
                unload(e); // 1. unused for too long
            } else {
                idle.push_back(&e);
            }
        }
    }
    stats_.requested = requested.size();

    // 2. Targets: what each request wants.
    std::unordered_map<Entry*, int> target;
    std::uint64_t total = 0;
    for (Entry* e : requested) {
        target[e] = e->wanted;
        total += e->level_bytes[static_cast<std::size_t>(e->wanted)];
    }
    for (Entry* e : idle) {
        total += e->resident_bytes;
    }
    // 3a. Over budget: idle assets go first, least recently used first.
    std::sort(idle.begin(), idle.end(), [](const Entry* a, const Entry* b) { return a->last_used < b->last_used; });
    std::vector<Entry*> evict;
    for (Entry* e : idle) {
        if (total <= budget_) {
            break;
        }
        total -= e->resident_bytes;
        evict.push_back(e);
    }
    // 3b. Then coarsen the smallest-on-screen requests, one level per pass.
    std::sort(requested.begin(), requested.end(),
              [](const Entry* a, const Entry* b) { return a->screen_px < b->screen_px; });
    bool changed = true;
    while (total > budget_ && changed) {
        changed = false;
        for (Entry* e : requested) {
            int& t = target[e];
            if (t >= 0 && t + 1 < e->levels) {
                total -= e->level_bytes[static_cast<std::size_t>(t)] - e->level_bytes[static_cast<std::size_t>(t + 1)];
                ++t;
                changed = true;
                if (total <= budget_) {
                    break;
                }
            }
        }
    }
    // 3c. Still over (even the coarsest levels don't fit): drop the smallest.
    for (Entry* e : requested) {
        if (total <= budget_) {
            break;
        }
        int& t = target[e];
        if (t >= 0) {
            total -= e->level_bytes[static_cast<std::size_t>(t)];
            t = -1;
        }
    }

    // 4. Apply: free memory first, then upgrades (largest on screen first) within the upload budget.
    for (Entry* e : evict) {
        unload(*e);
    }
    std::vector<Entry*> upgrades;
    for (Entry* e : requested) {
        const int t = target[e];
        if (t < 0) {
            unload(*e);
        } else if (e->resident >= 0 && t > e->resident) {
            load(*e, t); // coarser: fewer bytes
        } else if (e->resident < 0 || t < e->resident) {
            upgrades.push_back(e);
        }
    }
    std::sort(upgrades.begin(), upgrades.end(),
              [](const Entry* a, const Entry* b) { return a->screen_px > b->screen_px; });
    std::uint64_t uploaded = 0;
    for (Entry* e : upgrades) {
        const int t = target[e];
        const std::uint64_t bytes = e->level_bytes[static_cast<std::size_t>(t)];
        if (uploaded > 0 && uploaded + bytes > upload_budget_) {
            break; // the rest next frame
        }
        if (stats_.used - e->resident_bytes + bytes > budget_) {
            continue; // can't fit yet (a downgrade elsewhere frees room next frame)
        }
        if (load(*e, t)) {
            uploaded += bytes;
        }
    }

    // Reset this frame's requests.
    for (auto& [k, e] : entries_) {
        e.shown_wanted = e.wanted;
        e.shown_px = e.screen_px;
        e.wanted = -1;
        e.screen_px = 0.0f;
    }
    stats_.budget = budget_;
    stats_.peak = std::max(stats_.peak, stats_.used);
    stats_.meshes = 0;
    stats_.textures = 0;
    for (const auto& [k, e] : entries_) {
        if (e.resident >= 0) {
            (e.is_texture ? stats_.textures : stats_.meshes) += 1;
        }
    }
    ++frame_;
}

void ResidencyManager::invalidate(const core::Guid& guid) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.ref.guid == guid) {
            unload(it->second);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    ++generation_;
}

void ResidencyManager::clear() {
    for (auto& [k, e] : entries_) {
        unload(e);
    }
    entries_.clear();
    ++generation_;
}

std::vector<ResidencyRow> ResidencyManager::rows() const {
    std::vector<ResidencyRow> out;
    out.reserve(entries_.size());
    for (const auto& [k, e] : entries_) {
        ResidencyRow row;
        row.name = e.ref.path;
        row.texture = e.is_texture;
        row.resident = e.resident;
        row.levels = e.levels;
        row.wanted = e.shown_wanted;
        row.screen_px = e.shown_px;
        row.bytes = e.resident_bytes;
        row.idle_frames = frame_ - 1 - std::min(frame_ - 1, e.last_used);
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), [](const ResidencyRow& a, const ResidencyRow& b) { return a.bytes > b.bytes; });
    return out;
}

} // namespace f3d::render::raster
