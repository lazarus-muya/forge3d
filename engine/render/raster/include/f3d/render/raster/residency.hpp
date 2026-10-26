// SPDX-License-Identifier: Apache-2.0
#pragma once

// GPU residency of project assets (phases.md P3.5): which mesh LOD and which
// texture mips are in VRAM, chosen per frame from how large each asset appears
// on screen, under a VRAM budget, least-recently-used first out.
//
// The renderer asks for an asset with its on-screen size (mesh(), texture());
// it gets what is resident now (maybe coarser than wanted, or nothing on the
// first frame). end_frame() then settles the next state:
//   1. assets unused for `idle_frames` are evicted;
//   2. each requested asset wants the level matching its screen size;
//   3. while the wanted total exceeds the budget, idle resident assets are
//      evicted (oldest first), then the lowest-priority (smallest on screen)
//      requests are coarsened one level at a time, then dropped;
//   4. changes are applied: downgrades at once, upgrades within a per-frame
//      upload budget, largest on screen first.
// So the resident total never exceeds the budget after end_frame().

#include "f3d/assets/cook.hpp"
#include "f3d/assets/texture.hpp"
#include "f3d/core/asset_ref.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/render/rhi/device.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3d::render::raster {

/// Where residency gets CPU data. Called on the main thread; may load from disk.
struct ResidencySources {
    /// The cooked mesh a reference points at (guid + "#mesh/<n>"), or null.
    std::function<std::shared_ptr<const assets::CookedMesh>(const core::AssetRef&)> mesh;
    /// The cooked texture a reference points at (a texture asset, or an image of a
    /// cooked model: guid + "#image/<n>"), or null.
    std::function<std::shared_ptr<const assets::CookedTexture>(const core::AssetRef&)> texture;
};

/// A resident mesh LOD (vertices compacted to the ones that LOD uses).
struct GpuMeshLod {
    rhi::Buffer vertices; ///< MeshVertex
    rhi::Buffer uvs;      ///< glm::vec2 (zeros if the mesh has none)
    rhi::Buffer indices;  ///< u32
    std::uint32_t index_count = 0;
    std::uint32_t lod = 0;
    std::vector<assets::SubMesh> submeshes; ///< material ranges of this LOD (relative to `indices`)
};

/// A GPU texture holding mips `top_mip`.. of a cooked texture (sRGB formats for sRGB data).
[[nodiscard]] rhi::Texture upload_texture(rhi::Device& device, const assets::CookedTexture& texture,
                                          std::uint32_t top_mip, const std::string& name);

/// A resident texture: mips `top_mip`.. of the cooked texture.
struct GpuTexture {
    rhi::Texture texture;
    std::uint32_t top_mip = 0;
};

struct ResidencyStats {
    std::uint64_t budget = 0;
    std::uint64_t used = 0;
    std::uint64_t peak = 0;      ///< largest `used` seen after end_frame()
    std::size_t meshes = 0;      ///< resident
    std::size_t textures = 0;    ///< resident
    std::size_t requested = 0;   ///< assets asked for last frame
    std::uint64_t evictions = 0; ///< total
    std::uint64_t uploads = 0;   ///< total
};

/// One row of the residency panel.
struct ResidencyRow {
    std::string name;
    bool texture = false;
    int resident = -1; ///< resident level (LOD / top mip), -1: none
    int wanted = -1;   ///< level wanted last frame, -1: not requested
    int levels = 0;
    std::uint64_t bytes = 0;
    float screen_px = 0.0f;
    std::uint64_t idle_frames = 0;
};

class ResidencyManager {
public:
    ResidencyManager(rhi::Device& device, ResidencySources sources);
    ~ResidencyManager();

    void set_budget(std::uint64_t bytes) noexcept { budget_ = bytes; }
    [[nodiscard]] std::uint64_t budget() const noexcept { return budget_; }
    /// Bytes uploaded per frame at most (upgrades only).
    void set_upload_budget(std::uint64_t bytes) noexcept { upload_budget_ = bytes; }
    /// Frames an unused asset stays resident (if the budget allows).
    void set_idle_frames(std::uint64_t frames) noexcept { idle_frames_ = frames; }

    /// Requests an asset for this frame. `screen_px`: its on-screen radius in
    /// pixels. Returns what is resident now (possibly coarser), or null.
    const GpuMeshLod* mesh(const core::AssetRef& ref, float screen_px);
    const GpuTexture* texture(const core::AssetRef& ref, float screen_px);
    /// The cooked mesh's bounds (mesh space), for screen-size estimates; empty if unknown.
    [[nodiscard]] core::Bounds mesh_bounds(const core::AssetRef& ref);

    /// Settles residency for the next frame (see the header comment).
    void end_frame();
    /// Drops every resident copy of `guid` (re-imported: hot swap on the next request).
    void invalidate(const core::Guid& guid);
    void clear();

    [[nodiscard]] const ResidencyStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::vector<ResidencyRow> rows() const;
    /// Generation that changes whenever a resident resource is replaced (renderers drop caches).
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Level selection, exposed for tests: the mesh LOD for an on-screen radius,
    /// and the top mip for a texture of `width` px drawn over that radius.
    [[nodiscard]] static int lod_for(float screen_px, int lods) noexcept;
    [[nodiscard]] static int mip_for(float screen_px, std::uint32_t width, int mips) noexcept;

private:
    struct Entry {
        core::AssetRef ref;
        bool is_texture = false;
        std::shared_ptr<const assets::CookedMesh> cpu_mesh;
        int levels = 0;
        std::vector<std::uint64_t> level_bytes; ///< GPU bytes if level i is resident
        int resident = -1;
        std::uint64_t resident_bytes = 0;
        GpuMeshLod mesh;
        GpuTexture texture;
        int wanted = -1; ///< this frame
        float screen_px = 0.0f;
        std::uint64_t last_used = 0;
        std::uint32_t width = 0; ///< textures: width of mip 0
        int shown_wanted = -1;   ///< last frame's request, for rows()
        float shown_px = 0.0f;
    };
    Entry* entry(const core::AssetRef& ref, bool texture);
    bool load(Entry& e, int level);
    void unload(Entry& e);
    static std::string key(const core::AssetRef& ref, bool texture);

    rhi::Device& device_;
    ResidencySources sources_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t budget_ = 2048ull << 20;
    std::uint64_t upload_budget_ = 256ull << 20;
    std::uint64_t idle_frames_ = 300;
    std::uint64_t frame_ = 1;
    std::uint64_t generation_ = 0;
    ResidencyStats stats_;
};

} // namespace f3d::render::raster
