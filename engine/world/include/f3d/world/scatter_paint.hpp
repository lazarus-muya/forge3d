// SPDX-License-Identifier: Apache-2.0
#pragma once

// Painted scatter density (phases.md P7.1, density and erase brushes). A ScatterMask is
// a grid of 8-bit cells over a scatter's area: 255 keeps every candidate the rules
// allow, 0 removes them all, values between thin them out (deterministically, per
// candidate). A scatter without a mask is full ("Fill"), or empty if it is
// `paint_only`, until the first stroke creates one. Strokes are recorded like terrain
// strokes (one undo step, a compressed before/after diff of the touched cells), and
// masks are saved in .forge chunks.

#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"
#include "f3d/terrain/sculpt.hpp"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::world {

struct Scatter;

class ScatterMask {
public:
    ScatterMask(std::uint32_t width, std::uint32_t height, std::uint8_t fill);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint8_t at(std::uint32_t x, std::uint32_t z) const noexcept {
        return cells_[static_cast<std::size_t>(z) * width_ + x];
    }
    void set(std::uint32_t x, std::uint32_t z, std::uint8_t value) noexcept {
        cells_[static_cast<std::size_t>(z) * width_ + x] = value;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& cells() const noexcept { return cells_; }
    [[nodiscard]] std::vector<std::uint8_t>& cells() noexcept { return cells_; }
    /// Density (0..1) at (u, v) in [0, 1] over the area (u along +x, v along +z), bilinear.
    [[nodiscard]] float sample(float u, float v) const noexcept;

    /// Bumped by every change (call after writing cells).
    void touch() noexcept { ++revision_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    /// "F3SM", version, width, height, then the cells (little-endian uint32 header).
    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    [[nodiscard]] static std::optional<ScatterMask> deserialize(const std::vector<std::uint8_t>& bytes);

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::uint8_t> cells_;
    std::uint64_t revision_ = 1;
};

/// Mask cells for an area: about one per metre, at most 2048 along the longer side.
[[nodiscard]] glm::uvec2 mask_resolution(const glm::vec2& area) noexcept;

/// A new mask for a scatter: full, or empty if it is paint_only.
[[nodiscard]] std::shared_ptr<ScatterMask> new_scatter_mask(const Scatter& scatter);

/// The masks of a World's scatters, keyed by scatter entity GUID (registry context).
class ScatterMaskStore {
public:
    [[nodiscard]] std::shared_ptr<ScatterMask> find(const core::Guid& scatter) const;
    void set(const core::Guid& scatter, std::shared_ptr<ScatterMask> mask);
    void erase(const core::Guid& scatter);
    [[nodiscard]] const std::unordered_map<core::Guid, std::shared_ptr<ScatterMask>>& all() const noexcept {
        return map_;
    }

private:
    std::unordered_map<core::Guid, std::shared_ptr<ScatterMask>> map_;
};

[[nodiscard]] ScatterMaskStore& scatter_masks(scene::World& world);
/// The mask of a scatter entity, or null (none painted yet).
[[nodiscard]] std::shared_ptr<ScatterMask> scatter_mask_of(scene::World& world, entt::entity scatter);

/// The cells a stroke changed: a rectangle, before and (after XOR before), zstd-compressed.
struct MaskDiff {
    std::uint32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0; ///< inclusive
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> delta;
    bool empty = true;

    void apply_before(ScatterMask& mask) const;
    void apply_after(ScatterMask& mask) const;
    [[nodiscard]] std::size_t bytes() const noexcept { return sizeof(*this) + before.size() + delta.size(); }
};

/// Paints density into a scatter's mask along a stroke (world x/z points), adding
/// (paint) or removing (erase) density under the brush.
class ScatterPaintRecorder {
public:
    /// `centre`: the scatter's world position; `area` its extent. The mask must match it.
    ScatterPaintRecorder(std::shared_ptr<ScatterMask> mask, const glm::vec3& centre, const glm::vec2& area,
                         terrain::Brush brush, bool erase);

    void add(const terrain::StrokePoint& world_point);
    [[nodiscard]] const std::vector<terrain::StrokePoint>& points() const noexcept { return points_; }
    [[nodiscard]] const terrain::Brush& brush() const noexcept { return brush_; }
    [[nodiscard]] bool erase() const noexcept { return erase_; }
    [[nodiscard]] const std::shared_ptr<ScatterMask>& mask() const noexcept { return mask_; }

    /// The diff of everything painted so far.
    [[nodiscard]] MaskDiff finish() const;
    /// Puts the mask back as it was before the stroke.
    void restore();

private:
    void apply_dab(const terrain::StrokePoint& dab);

    std::shared_ptr<ScatterMask> mask_;
    glm::vec3 centre_;
    glm::vec2 area_;
    terrain::Brush brush_;
    bool erase_;
    terrain::DabSpacer spacer_;
    std::vector<terrain::StrokePoint> points_;
    std::vector<terrain::StrokePoint> dabs_;
    std::vector<std::uint8_t> before_; ///< the whole mask before the stroke
    bool touched_ = false;
    std::uint32_t x0_ = 0, z0_ = 0, x1_ = 0, z1_ = 0;
};

/// The .forge chunks of every scatter mask ("scatter/<guid>/mask").
[[nodiscard]] std::map<std::string, std::vector<std::uint8_t>> scatter_chunks(scene::World& world);
/// Restores the masks of the world's scatters from saved chunks.
void restore_scatter_chunks(scene::World& world, const std::map<std::string, std::vector<std::uint8_t>>& chunks);

} // namespace f3d::world
