// SPDX-License-Identifier: Apache-2.0
#include "f3d/terrain/sculpt.hpp"

#include "f3d/core/image.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/terrain/erosion.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace f3d::terrain {
namespace {

constexpr std::array<std::string_view, 8> kToolNames = {"raise", "lower", "smooth", "flatten",
                                                        "terrace", "noise", "stamp", "erode"};
constexpr std::array<std::string_view, 4> kFalloffNames = {"smooth", "linear", "sphere", "constant"};

constexpr std::uint32_t kBlock = kTileQuads + 1; ///< samples per tile block side
constexpr std::size_t kBlockSamples = static_cast<std::size_t>(kBlock) * kBlock;

/// A tile's 257 x 257 block out of a full-size heights array laid out like `hf`.
std::vector<float> read_block(const Heightfield& hf, const std::vector<float>& heights, std::uint32_t tile) {
    const std::uint32_t tx = tile % hf.tiles();
    const std::uint32_t tz = tile / hf.tiles();
    std::vector<float> out(kBlockSamples);
    for (std::uint32_t z = 0; z < kBlock; ++z) {
        const std::size_t row = static_cast<std::size_t>(tz * kTileQuads + z) * hf.samples() + tx * kTileQuads;
        std::memcpy(out.data() + static_cast<std::size_t>(z) * kBlock, heights.data() + row, kBlock * sizeof(float));
    }
    return out;
}

std::vector<float> read_block(const Heightfield& hf, std::uint32_t tile) {
    return read_block(hf, hf.heights(), tile);
}

void write_block(Heightfield& hf, std::uint32_t tile, const std::vector<float>& block) {
    const std::uint32_t tx = tile % hf.tiles();
    const std::uint32_t tz = tile / hf.tiles();
    std::vector<float>& heights = hf.heights();
    for (std::uint32_t z = 0; z < kBlock; ++z) {
        const std::size_t row = static_cast<std::size_t>(tz * kTileQuads + z) * hf.samples() + tx * kTileQuads;
        std::memcpy(heights.data() + row, block.data() + static_cast<std::size_t>(z) * kBlock, kBlock * sizeof(float));
    }
    const std::uint32_t x0 = tx * kTileQuads;
    const std::uint32_t z0 = tz * kTileQuads;
    hf.mark_dirty(x0, z0, x0 + kTileQuads, z0 + kTileQuads);
    hf.update_bounds(x0, z0, x0 + kTileQuads, z0 + kTileQuads);
}

std::vector<std::uint8_t> compress(const void* data, std::size_t size) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(size));
    const std::size_t written = ZSTD_compress(out.data(), out.size(), data, size, 1);
    out.resize(ZSTD_isError(written) ? 0 : written);
    return out;
}

std::vector<float> decompress_block(const std::vector<std::uint8_t>& data) {
    std::vector<float> out(kBlockSamples, 0.0f);
    const std::size_t read = ZSTD_decompress(out.data(), kBlockSamples * sizeof(float), data.data(), data.size());
    if (ZSTD_isError(read) || read != kBlockSamples * sizeof(float)) {
        std::fill(out.begin(), out.end(), 0.0f); // corrupt diffs can't happen in memory; stay defined anyway
    }
    return out;
}

/// a XOR b, bit for bit (so before ^ delta = after exactly).
std::vector<float> xor_blocks(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint32_t ua = 0;
        std::uint32_t ub = 0;
        std::memcpy(&ua, &a[i], sizeof(ua));
        std::memcpy(&ub, &b[i], sizeof(ub));
        const std::uint32_t x = ua ^ ub;
        std::memcpy(&out[i], &x, sizeof(x));
    }
    return out;
}

/// First and last tile containing samples s0..s1 (a sample on a tile edge belongs to both).
std::uint32_t first_tile(std::uint32_t s) noexcept {
    return s == 0 ? 0u : (s - 1) / kTileQuads;
}
std::uint32_t last_tile(std::uint32_t s, std::uint32_t tiles) noexcept {
    return std::min(s / kTileQuads, tiles - 1);
}

} // namespace

std::string_view to_string(BrushTool tool) noexcept {
    return kToolNames[static_cast<std::size_t>(tool)];
}

std::string_view to_string(BrushFalloff falloff) noexcept {
    return kFalloffNames[static_cast<std::size_t>(falloff)];
}

bool parse(std::string_view text, BrushTool& tool) noexcept {
    for (std::size_t i = 0; i < kToolNames.size(); ++i) {
        if (kToolNames[i] == text) {
            tool = static_cast<BrushTool>(i);
            return true;
        }
    }
    return false;
}

bool parse(std::string_view text, BrushFalloff& falloff) noexcept {
    for (std::size_t i = 0; i < kFalloffNames.size(); ++i) {
        if (kFalloffNames[i] == text) {
            falloff = static_cast<BrushFalloff>(i);
            return true;
        }
    }
    return false;
}

float falloff_weight(BrushFalloff falloff, float d) noexcept {
    d = std::clamp(d, 0.0f, 1.0f);
    switch (falloff) {
    case BrushFalloff::Smooth:
        return 1.0f - d * d * (3.0f - 2.0f * d);
    case BrushFalloff::Linear:
        return 1.0f - d;
    case BrushFalloff::Sphere:
        return std::sqrt(1.0f - d * d);
    case BrushFalloff::Constant:
        return 1.0f;
    }
    return 0.0f;
}

// --- Stamp -----------------------------------------------------------------------------

float Stamp::sample(float u, float v) const noexcept {
    if (width == 0 || height == 0 || u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return 0.0f;
    }
    const float fx = u * static_cast<float>(width - 1);
    const float fy = v * static_cast<float>(height - 1);
    const auto x0 = std::min(static_cast<std::uint32_t>(fx), width - 1);
    const auto y0 = std::min(static_cast<std::uint32_t>(fy), height - 1);
    const std::uint32_t x1 = std::min(x0 + 1, width - 1);
    const std::uint32_t y1 = std::min(y0 + 1, height - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto at = [&](std::uint32_t x, std::uint32_t y) { return values[static_cast<std::size_t>(y) * width + x]; };
    const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return a + (b - a) * ty;
}

std::shared_ptr<const Stamp> load_stamp(std::string_view path, const core::vfs::FileSystem* files) {
    if (path.empty()) {
        return nullptr;
    }
    std::vector<std::uint8_t> bytes;
    if (files && path.find("://") != std::string_view::npos) {
        auto read = files->read(path);
        if (!read) {
            return nullptr;
        }
        bytes = std::move(*read);
    } else {
        std::ifstream in(std::string(path), std::ios::binary);
        if (!in) {
            return nullptr;
        }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::optional<core::Image> image = core::decode_image(bytes);
    if (!image || image->width == 0 || image->height == 0) {
        return nullptr;
    }
    auto stamp = std::make_shared<Stamp>();
    stamp->width = image->width;
    stamp->height = image->height;
    stamp->values.resize(static_cast<std::size_t>(image->width) * image->height);
    for (std::size_t i = 0; i < stamp->values.size(); ++i) {
        stamp->values[i] = static_cast<float>(image->rgba[i * 4]) / 255.0f;
    }
    return stamp;
}

// --- Dab spacing -----------------------------------------------------------------------

void DabSpacer::add(const StrokePoint& point, std::vector<StrokePoint>& dabs) {
    if (!started_) {
        started_ = true;
        previous_ = point;
        travelled_ = 0.0f;
        dabs.push_back(point);
        return;
    }
    const glm::vec2 delta = point.position - previous_.position;
    const float length = glm::length(delta);
    float along = 0.0f; // of this segment, consumed
    while (length > 0.0f && travelled_ + (length - along) >= step_) {
        along += step_ - travelled_;
        travelled_ = 0.0f;
        const float t = along / length;
        dabs.push_back({previous_.position + delta * t, previous_.pressure + (point.pressure - previous_.pressure) * t});
    }
    travelled_ += length - along;
    previous_ = point;
}

// --- Tile diffs ------------------------------------------------------------------------

void TileDiff::apply_before(Heightfield& heightfield) const {
    for (const Tile& tile : tiles) {
        write_block(heightfield, tile.index, decompress_block(tile.before));
    }
}

void TileDiff::apply_after(Heightfield& heightfield) const {
    for (const Tile& tile : tiles) {
        write_block(heightfield, tile.index, xor_blocks(decompress_block(tile.before), decompress_block(tile.delta)));
    }
}

TileDiff diff_tiles(const Heightfield& layout, const std::vector<float>& before, const std::vector<float>& after) {
    F3D_ZONE;
    TileDiff diff;
    const std::uint32_t tiles = layout.tiles() * layout.tiles();
    for (std::uint32_t tile = 0; tile < tiles; ++tile) {
        const std::vector<float> a = read_block(layout, before, tile);
        const std::vector<float> b = read_block(layout, after, tile);
        if (std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0) {
            continue;
        }
        const std::vector<float> delta = xor_blocks(a, b);
        TileDiff::Tile t;
        t.index = tile;
        t.before = compress(a.data(), a.size() * sizeof(float));
        t.delta = compress(delta.data(), delta.size() * sizeof(float));
        diff.tiles.push_back(std::move(t));
    }
    return diff;
}

std::size_t TileDiff::bytes() const noexcept {
    std::size_t total = 0;
    for (const Tile& tile : tiles) {
        total += sizeof(Tile) + tile.before.size() + tile.delta.size();
    }
    return total;
}

// --- Stroke ----------------------------------------------------------------------------

StrokeRecorder::StrokeRecorder(std::shared_ptr<Heightfield> heightfield, const Terrain& terrain, Brush brush,
                               std::shared_ptr<const Stamp> stamp)
    : heightfield_(std::move(heightfield)), terrain_(terrain), brush_(std::move(brush)), stamp_(std::move(stamp)),
      spacer_(std::max(brush_.radius, 1e-3f) * std::clamp(brush_.spacing, 0.01f, 4.0f)),
      slot_of_tile_(static_cast<std::size_t>(heightfield_->tiles()) * heightfield_->tiles(), -1) {}

void StrokeRecorder::add(const StrokePoint& point) {
    F3D_ZONE;
    points_.push_back(point);
    dabs_.clear();
    spacer_.add(point, dabs_);
    for (const StrokePoint& dab : dabs_) {
        apply_dab(dab);
    }
}

void StrokeRecorder::capture_tiles(std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1) {
    const std::uint32_t tiles = heightfield_->tiles();
    for (std::uint32_t tz = first_tile(z0); tz <= last_tile(z1, tiles); ++tz) {
        for (std::uint32_t tx = first_tile(x0); tx <= last_tile(x1, tiles); ++tx) {
            const std::uint32_t tile = tz * tiles + tx;
            if (slot_of_tile_[tile] >= 0) {
                continue;
            }
            slot_of_tile_[tile] = static_cast<std::int32_t>(captured_.size());
            captured_.push_back(tile);
            before_.push_back(read_block(*heightfield_, tile));
        }
    }
}

void StrokeRecorder::apply_dab(const StrokePoint& dab) {
    Heightfield& hf = *heightfield_;
    const std::uint32_t n = hf.samples();
    const float last = static_cast<float>(n - 1);
    const float spacing = hf.spacing();
    const float cx = (dab.position.x / hf.size() + 0.5f) * last;
    const float cz = (dab.position.y / hf.size() + 0.5f) * last;
    const float rs = brush_.radius / spacing; // radius in samples
    if (!(rs > 0.0f)) {
        return;
    }
    const float fx0 = std::max(std::ceil(cx - rs), 0.0f);
    const float fx1 = std::min(std::floor(cx + rs), last);
    const float fz0 = std::max(std::ceil(cz - rs), 0.0f);
    const float fz1 = std::min(std::floor(cz + rs), last);
    if (fx0 > fx1 || fz0 > fz1) {
        return; // entirely off the terrain
    }
    const auto x0 = static_cast<std::uint32_t>(fx0);
    const auto x1 = static_cast<std::uint32_t>(fx1);
    const auto z0 = static_cast<std::uint32_t>(fz0);
    const auto z1 = static_cast<std::uint32_t>(fz1);
    capture_tiles(x0, z0, x1, z1);

    const float strength = std::clamp(brush_.strength, 0.0f, 1.0f) * std::clamp(dab.pressure, 0.0f, 1.0f);
    const float amount = strength * brush_.radius * 0.05f; // metres per dab at the centre
    if (brush_.tool == BrushTool::Flatten && !have_flatten_height_) {
        flatten_height_ = brush_.use_target ? brush_.target_height : hf.height_at(dab.position.x, dab.position.y);
        have_flatten_height_ = true;
    }

    // Smooth: a box blur of the region (the kernel grows with the brush), from the
    // heights before this dab.
    const std::uint32_t w = x1 - x0 + 1;
    if (brush_.tool == BrushTool::Smooth) {
        const auto k = static_cast<std::uint32_t>(std::clamp(rs * 0.15f, 1.0f, 8.0f));
        const std::uint32_t bz0 = z0 >= k ? z0 - k : 0;
        const std::uint32_t bz1 = std::min(z1 + k, n - 1);
        std::vector<float> rows(static_cast<std::size_t>(w) * (bz1 - bz0 + 1));
        for (std::uint32_t z = bz0; z <= bz1; ++z) {
            for (std::uint32_t x = x0; x <= x1; ++x) {
                const std::uint32_t a = x >= k ? x - k : 0;
                const std::uint32_t b = std::min(x + k, n - 1);
                float sum = 0.0f;
                for (std::uint32_t i = a; i <= b; ++i) {
                    sum += hf.at(i, z);
                }
                rows[static_cast<std::size_t>(z - bz0) * w + (x - x0)] = sum / static_cast<float>(b - a + 1);
            }
        }
        scratch_.assign(static_cast<std::size_t>(w) * (z1 - z0 + 1), 0.0f);
        for (std::uint32_t z = z0; z <= z1; ++z) {
            const std::uint32_t a = z >= k ? z - k : 0;
            const std::uint32_t b = std::min(z + k, n - 1);
            for (std::uint32_t x = x0; x <= x1; ++x) {
                float sum = 0.0f;
                for (std::uint32_t i = a; i <= b; ++i) {
                    sum += rows[static_cast<std::size_t>(i - bz0) * w + (x - x0)];
                }
                scratch_[static_cast<std::size_t>(z - z0) * w + (x - x0)] = sum / static_cast<float>(b - a + 1);
            }
        }
    }

    // Erode: run erosion on the dab's neighbourhood, then blend it in by the falloff.
    if (brush_.tool == BrushTool::Erode) {
        const auto margin = static_cast<std::uint32_t>(std::max(rs * 0.5f, 4.0f));
        const std::uint32_t ex0 = x0 >= margin ? x0 - margin : 0;
        const std::uint32_t ez0 = z0 >= margin ? z0 - margin : 0;
        const std::uint32_t ex1 = std::min(x1 + margin, n - 1);
        const std::uint32_t ez1 = std::min(z1 + margin, n - 1);
        const std::uint32_t ew = ex1 - ex0 + 1;
        const std::uint32_t eh = ez1 - ez0 + 1;
        scratch_.resize(static_cast<std::size_t>(ew) * eh);
        for (std::uint32_t z = 0; z < eh; ++z) {
            for (std::uint32_t x = 0; x < ew; ++x) {
                scratch_[static_cast<std::size_t>(z) * ew + x] = hf.at(ex0 + x, ez0 + z);
            }
        }
        ErosionSettings erosion;
        erosion.iterations = std::max(brush_.erosion_iterations, 1u);
        erosion.seed = brush_.noise_seed + static_cast<std::uint32_t>(dabs_applied_);
        erosion.rain = 0.03f; // more water than a global pass: few iterations
        erode(scratch_, ew, eh, spacing, erosion, terrain_.min_height, terrain_.max_height);
        for (std::uint32_t z = z0; z <= z1; ++z) {
            for (std::uint32_t x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) - cx) / rs;
                const float dz = (static_cast<float>(z) - cz) / rs;
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d > 1.0f) {
                    continue;
                }
                const float eroded = scratch_[static_cast<std::size_t>(z - ez0) * ew + (x - ex0)];
                const float h = hf.at(x, z);
                hf.set(x, z, h + (eroded - h) * falloff_weight(brush_.falloff, d) * strength);
            }
        }
        hf.mark_dirty(x0, z0, x1, z1);
        hf.update_bounds(x0, z0, x1, z1);
        ++dabs_applied_;
        return;
    }

    const float cos_r = std::cos(brush_.stamp_rotation);
    const float sin_r = std::sin(brush_.stamp_rotation);
    const float half = hf.size() * 0.5f;
    const float step = brush_.terrace_step;
    const float noise_scale = std::max(brush_.noise_scale, 1e-3f);
    for (std::uint32_t z = z0; z <= z1; ++z) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) - cx) / rs;
            const float dz = (static_cast<float>(z) - cz) / rs;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > 1.0f) {
                continue;
            }
            const float weight = falloff_weight(brush_.falloff, d);
            float h = hf.at(x, z);
            switch (brush_.tool) {
            case BrushTool::Raise:
                h += amount * weight;
                break;
            case BrushTool::Lower:
                h -= amount * weight;
                break;
            case BrushTool::Smooth:
                h += (scratch_[static_cast<std::size_t>(z - z0) * w + (x - x0)] - h) * (weight * strength);
                break;
            case BrushTool::Flatten:
                h += (flatten_height_ - h) * (weight * strength);
                break;
            case BrushTool::Terrace:
                if (step > 0.0f) {
                    h += (std::round(h / step) * step - h) * (weight * strength);
                }
                break;
            case BrushTool::Noise: {
                const float lx = static_cast<float>(x) * spacing - half;
                const float lz = static_cast<float>(z) * spacing - half;
                h += gradient_noise(lx / noise_scale, lz / noise_scale, brush_.noise_seed) * 1.4f * amount * weight;
                break;
            }
            case BrushTool::Erode: // handled above
                break;
            case BrushTool::Stamp:
                if (stamp_) {
                    const float u = (dx * cos_r + dz * sin_r) * 0.5f + 0.5f;
                    const float v = (-dx * sin_r + dz * cos_r) * 0.5f + 0.5f;
                    h += stamp_->sample(u, v) * brush_.stamp_height * strength * weight;
                }
                break;
            }
            hf.set(x, z, std::clamp(h, terrain_.min_height, terrain_.max_height));
        }
    }
    hf.mark_dirty(x0, z0, x1, z1);
    hf.update_bounds(x0, z0, x1, z1);
    ++dabs_applied_;
}

TileDiff StrokeRecorder::finish() const {
    F3D_ZONE;
    TileDiff diff;
    diff.tiles.reserve(captured_.size());
    for (std::size_t i = 0; i < captured_.size(); ++i) {
        const std::vector<float> after = read_block(*heightfield_, captured_[i]);
        const std::vector<float> delta = xor_blocks(before_[i], after);
        TileDiff::Tile tile;
        tile.index = captured_[i];
        tile.before = compress(before_[i].data(), before_[i].size() * sizeof(float));
        tile.delta = compress(delta.data(), delta.size() * sizeof(float));
        diff.tiles.push_back(std::move(tile));
    }
    return diff;
}

void StrokeRecorder::restore() {
    for (std::size_t i = 0; i < captured_.size(); ++i) {
        write_block(*heightfield_, captured_[i], before_[i]);
    }
}

} // namespace f3d::terrain
