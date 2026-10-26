// SPDX-License-Identifier: Apache-2.0
#include "f3d/world/scatter_paint.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/world/scatter.hpp"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace f3d::world {
namespace {

constexpr std::uint32_t kMaskMagic = 0x4d533346u; // "F3SM"
constexpr std::uint32_t kMaskVersion = 1;

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(data.size()));
    const std::size_t written = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 1);
    out.resize(ZSTD_isError(written) ? 0 : written);
    return out;
}

std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& data, std::size_t size) {
    std::vector<std::uint8_t> out(size, 0);
    const std::size_t read = ZSTD_decompress(out.data(), size, data.data(), data.size());
    if (ZSTD_isError(read) || read != size) {
        std::fill(out.begin(), out.end(), std::uint8_t{0});
    }
    return out;
}

void put(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

std::uint32_t get(const std::vector<std::uint8_t>& in, std::size_t at) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(in[at + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

/// The cells of rectangle [x0, x1] x [z0, z1] of `cells` (row width `width`).
std::vector<std::uint8_t> read_rect(const std::vector<std::uint8_t>& cells, std::uint32_t width, std::uint32_t x0,
                                    std::uint32_t z0, std::uint32_t x1, std::uint32_t z1) {
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(x1 - x0 + 1) * (z1 - z0 + 1));
    for (std::uint32_t z = z0; z <= z1; ++z) {
        const auto* row = cells.data() + static_cast<std::size_t>(z) * width;
        out.insert(out.end(), row + x0, row + x1 + 1);
    }
    return out;
}

void write_rect(ScatterMask& mask, std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1,
                const std::vector<std::uint8_t>& rect) {
    const std::size_t w = x1 - x0 + 1;
    for (std::uint32_t z = z0; z <= z1; ++z) {
        std::memcpy(mask.cells().data() + static_cast<std::size_t>(z) * mask.width() + x0,
                    rect.data() + (z - z0) * w, w);
    }
    mask.touch();
}

} // namespace

ScatterMask::ScatterMask(std::uint32_t width, std::uint32_t height, std::uint8_t fill)
    : width_(std::max(width, 1u)), height_(std::max(height, 1u)),
      cells_(static_cast<std::size_t>(width_) * height_, fill) {}

float ScatterMask::sample(float u, float v) const noexcept {
    const float fx = std::clamp(u * static_cast<float>(width_) - 0.5f, 0.0f, static_cast<float>(width_ - 1));
    const float fz = std::clamp(v * static_cast<float>(height_) - 0.5f, 0.0f, static_cast<float>(height_ - 1));
    const auto x0 = static_cast<std::uint32_t>(fx);
    const auto z0 = static_cast<std::uint32_t>(fz);
    const std::uint32_t x1 = std::min(x0 + 1, width_ - 1);
    const std::uint32_t z1 = std::min(z0 + 1, height_ - 1);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);
    const float a = static_cast<float>(at(x0, z0)) + (static_cast<float>(at(x1, z0)) - static_cast<float>(at(x0, z0))) * tx;
    const float b = static_cast<float>(at(x0, z1)) + (static_cast<float>(at(x1, z1)) - static_cast<float>(at(x0, z1))) * tx;
    return (a + (b - a) * tz) / 255.0f;
}

std::vector<std::uint8_t> ScatterMask::serialize() const {
    std::vector<std::uint8_t> out;
    out.reserve(16 + cells_.size());
    put(out, kMaskMagic);
    put(out, kMaskVersion);
    put(out, width_);
    put(out, height_);
    out.insert(out.end(), cells_.begin(), cells_.end());
    return out;
}

std::optional<ScatterMask> ScatterMask::deserialize(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 16 || get(bytes, 0) != kMaskMagic || get(bytes, 4) != kMaskVersion) {
        return std::nullopt;
    }
    const std::uint32_t width = get(bytes, 8);
    const std::uint32_t height = get(bytes, 12);
    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        bytes.size() - 16 != static_cast<std::size_t>(width) * height) {
        return std::nullopt;
    }
    ScatterMask mask(width, height, 0);
    std::memcpy(mask.cells_.data(), bytes.data() + 16, mask.cells_.size());
    return mask;
}

glm::uvec2 mask_resolution(const glm::vec2& area) noexcept {
    const float longest = std::max({area.x, area.y, 1.0f});
    const float cell = std::max(1.0f, longest / 2048.0f);
    return {static_cast<std::uint32_t>(std::clamp(std::ceil(area.x / cell), 1.0f, 2048.0f)),
            static_cast<std::uint32_t>(std::clamp(std::ceil(area.y / cell), 1.0f, 2048.0f))};
}

std::shared_ptr<ScatterMask> new_scatter_mask(const Scatter& scatter) {
    const glm::uvec2 res = mask_resolution(scatter.area);
    return std::make_shared<ScatterMask>(res.x, res.y, scatter.paint_only ? 0 : 255);
}

// --- Store -----------------------------------------------------------------------------------

std::shared_ptr<ScatterMask> ScatterMaskStore::find(const core::Guid& scatter) const {
    const auto it = map_.find(scatter);
    return it == map_.end() ? nullptr : it->second;
}

void ScatterMaskStore::set(const core::Guid& scatter, std::shared_ptr<ScatterMask> mask) {
    map_[scatter] = std::move(mask);
}

void ScatterMaskStore::erase(const core::Guid& scatter) {
    map_.erase(scatter);
}

ScatterMaskStore& scatter_masks(scene::World& world) {
    auto& ctx = world.registry().ctx();
    if (!ctx.contains<ScatterMaskStore>()) {
        ctx.emplace<ScatterMaskStore>();
    }
    return ctx.get<ScatterMaskStore>();
}

std::shared_ptr<ScatterMask> scatter_mask_of(scene::World& world, entt::entity scatter) {
    if (!world.valid(scatter) || !world.registry().all_of<Scatter>(scatter)) {
        return nullptr;
    }
    return scatter_masks(world).find(world.guid(scatter));
}

// --- Diffs and strokes -----------------------------------------------------------------------

void MaskDiff::apply_before(ScatterMask& mask) const {
    if (empty || x1 >= mask.width() || z1 >= mask.height()) {
        return;
    }
    const std::size_t size = static_cast<std::size_t>(x1 - x0 + 1) * (z1 - z0 + 1);
    write_rect(mask, x0, z0, x1, z1, decompress(before, size));
}

void MaskDiff::apply_after(ScatterMask& mask) const {
    if (empty || x1 >= mask.width() || z1 >= mask.height()) {
        return;
    }
    const std::size_t size = static_cast<std::size_t>(x1 - x0 + 1) * (z1 - z0 + 1);
    std::vector<std::uint8_t> rect = decompress(before, size);
    const std::vector<std::uint8_t> change = decompress(delta, size);
    for (std::size_t i = 0; i < size; ++i) {
        rect[i] ^= change[i];
    }
    write_rect(mask, x0, z0, x1, z1, rect);
}

ScatterPaintRecorder::ScatterPaintRecorder(std::shared_ptr<ScatterMask> mask, const glm::vec3& centre,
                                           const glm::vec2& area, terrain::Brush brush, bool erase)
    : mask_(std::move(mask)), centre_(centre), area_(glm::max(area, glm::vec2(1e-3f))), brush_(std::move(brush)),
      erase_(erase), spacer_(std::max(brush_.radius, 1e-3f) * std::clamp(brush_.spacing, 0.01f, 4.0f)),
      before_(mask_->cells()) {}

void ScatterPaintRecorder::add(const terrain::StrokePoint& world_point) {
    points_.push_back(world_point);
    dabs_.clear();
    spacer_.add(world_point, dabs_);
    for (const terrain::StrokePoint& dab : dabs_) {
        apply_dab(dab);
    }
}

void ScatterPaintRecorder::apply_dab(const terrain::StrokePoint& dab) {
    ScatterMask& mask = *mask_;
    const glm::vec2 cell = area_ / glm::vec2(static_cast<float>(mask.width()), static_cast<float>(mask.height()));
    const glm::vec2 low = glm::vec2(centre_.x, centre_.z) - area_ * 0.5f;
    // Cell coordinates of the dab centre and its radius.
    const float cx = (dab.position.x - low.x) / cell.x - 0.5f;
    const float cz = (dab.position.y - low.y) / cell.y - 0.5f;
    const float rx = brush_.radius / cell.x;
    const float rz = brush_.radius / cell.y;
    const float fx0 = std::max(std::ceil(cx - rx), 0.0f);
    const float fx1 = std::min(std::floor(cx + rx), static_cast<float>(mask.width() - 1));
    const float fz0 = std::max(std::ceil(cz - rz), 0.0f);
    const float fz1 = std::min(std::floor(cz + rz), static_cast<float>(mask.height() - 1));
    if (!(brush_.radius > 0.0f) || fx0 > fx1 || fz0 > fz1) {
        return;
    }
    const auto x0 = static_cast<std::uint32_t>(fx0);
    const auto x1 = static_cast<std::uint32_t>(fx1);
    const auto z0 = static_cast<std::uint32_t>(fz0);
    const auto z1 = static_cast<std::uint32_t>(fz1);
    const float strength = std::clamp(brush_.strength, 0.0f, 1.0f) * std::clamp(dab.pressure, 0.0f, 1.0f);
    for (std::uint32_t z = z0; z <= z1; ++z) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) - cx) / rx;
            const float dz = (static_cast<float>(z) - cz) / rz;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > 1.0f) {
                continue;
            }
            const float a = std::clamp(terrain::falloff_weight(brush_.falloff, d) * strength, 0.0f, 1.0f);
            const float v = static_cast<float>(mask.at(x, z));
            const float next = erase_ ? v * (1.0f - a) : v + (255.0f - v) * a;
            mask.set(x, z, static_cast<std::uint8_t>(std::lround(std::clamp(next, 0.0f, 255.0f))));
        }
    }
    if (!touched_) {
        x0_ = x0;
        z0_ = z0;
        x1_ = x1;
        z1_ = z1;
        touched_ = true;
    } else {
        x0_ = std::min(x0_, x0);
        z0_ = std::min(z0_, z0);
        x1_ = std::max(x1_, x1);
        z1_ = std::max(z1_, z1);
    }
    mask.touch();
}

MaskDiff ScatterPaintRecorder::finish() const {
    F3D_ZONE;
    MaskDiff diff;
    if (!touched_) {
        return diff;
    }
    diff.x0 = x0_;
    diff.z0 = z0_;
    diff.x1 = x1_;
    diff.z1 = z1_;
    const std::vector<std::uint8_t> old = read_rect(before_, mask_->width(), x0_, z0_, x1_, z1_);
    std::vector<std::uint8_t> now = read_rect(mask_->cells(), mask_->width(), x0_, z0_, x1_, z1_);
    for (std::size_t i = 0; i < now.size(); ++i) {
        now[i] ^= old[i];
    }
    diff.before = compress(old);
    diff.delta = compress(now);
    diff.empty = false;
    return diff;
}

void ScatterPaintRecorder::restore() {
    mask_->cells() = before_;
    mask_->touch();
}

// --- Chunks ----------------------------------------------------------------------------------

std::map<std::string, std::vector<std::uint8_t>> scatter_chunks(scene::World& world) {
    std::map<std::string, std::vector<std::uint8_t>> out;
    for (const entt::entity e : world.registry().view<Scatter>()) {
        if (const auto mask = scatter_mask_of(world, e)) {
            out["scatter/" + world.guid(e).to_string() + "/mask"] = mask->serialize();
        }
    }
    return out;
}

void restore_scatter_chunks(scene::World& world, const std::map<std::string, std::vector<std::uint8_t>>& chunks) {
    for (const entt::entity e : world.registry().view<Scatter>()) {
        const core::Guid guid = world.guid(e);
        const auto it = chunks.find("scatter/" + guid.to_string() + "/mask");
        if (it == chunks.end()) {
            continue;
        }
        if (auto mask = ScatterMask::deserialize(it->second)) {
            scatter_masks(world).set(guid, std::make_shared<ScatterMask>(std::move(*mask)));
        } else {
            F3D_LOG_WARN("Scatter {}: its saved painting is damaged; skipped", world.name(e));
        }
    }
}

} // namespace f3d::world
