// SPDX-License-Identifier: Apache-2.0
#pragma once

// Material layers (phases.md P6.4): the rules that place layers by height and slope,
// and the paint brush that overrides them, both writing the splat maps (Splatmap in
// terrain.hpp). Painting records the splat tiles it changed (SplatDiff), like sculpting.

#include "f3d/terrain/sculpt.hpp"
#include "f3d/terrain/terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace f3d::terrain {

/// Weight (0..1) of a rule at a height (metres) and slope (degrees).
[[nodiscard]] float rule_weight(const LayerRule& rule, float height, float slope_degrees) noexcept;

/// The splat tiles some change touched: before, and after XOR before, zstd-compressed
/// (a block is a tile's texels in every map).
struct SplatDiff {
    struct Tile {
        std::uint32_t index = 0;
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> delta;
    };
    std::uint32_t maps = 0; ///< the block layout's map count
    std::vector<Tile> tiles;

    void apply_before(Splatmap& splat) const;
    void apply_after(Splatmap& splat) const;
    [[nodiscard]] std::size_t bytes() const noexcept;
};

/// A copy of every splat tile's block (to diff against later).
[[nodiscard]] std::vector<std::vector<std::uint8_t>> splat_blocks(const Splatmap& splat);
/// The diff between `before` (splat_blocks) and the splat now, for the tiles that differ.
[[nodiscard]] SplatDiff diff_splat(const Splatmap& splat, const std::vector<std::vector<std::uint8_t>>& before);

/// Bakes the layers' rules into the splat maps (all of them): layer 0 is the base and
/// each later layer with a rule covers what is below by its rule weight.
void apply_rules(const Heightfield& heights, const Terrain& terrain, Splatmap& splat);

/// Blends `amount` (0..1) of `layer` into texel (x, z), scaling the other layers down
/// (the weights keep summing to 255). The splat must hold the layer.
void paint_texel(Splatmap& splat, std::uint32_t x, std::uint32_t z, std::uint32_t layer, float amount);

/// Reorders the splat weights after the layers were reordered, added or removed:
/// new layer i takes the weights of old layer `source[i]` (-1: a new layer, weight 0).
/// Weight of layers that are gone is shared among the rest by their weights (texels
/// left with nothing become the base layer).
void remap_splat(Splatmap& splat, const std::vector<std::int32_t>& source);

/// Paints one layer (or erases it) along a stroke and records the splat tiles changed.
/// `brush`: radius, strength, falloff and spacing are used.
class PaintRecorder {
public:
    PaintRecorder(std::shared_ptr<Heightfield> heightfield, const Terrain& terrain, Brush brush, std::uint32_t layer,
                  bool erase);

    void add(const StrokePoint& point);
    [[nodiscard]] const std::vector<StrokePoint>& points() const noexcept { return points_; }
    [[nodiscard]] const Brush& brush() const noexcept { return brush_; }
    [[nodiscard]] std::uint32_t layer() const noexcept { return layer_; }
    [[nodiscard]] bool erase() const noexcept { return erase_; }
    [[nodiscard]] const std::shared_ptr<Heightfield>& heightfield() const noexcept { return heightfield_; }

    [[nodiscard]] SplatDiff finish() const;
    void restore();

private:
    void apply_dab(const StrokePoint& dab);

    std::shared_ptr<Heightfield> heightfield_;
    Brush brush_;
    std::uint32_t layer_;
    std::uint32_t layer_count_;
    bool erase_;
    DabSpacer spacer_;
    std::vector<StrokePoint> points_;
    std::vector<StrokePoint> dabs_;
    std::vector<std::int32_t> slot_of_tile_;
    std::vector<std::uint32_t> captured_;
    std::vector<std::vector<std::uint8_t>> before_;
};

} // namespace f3d::terrain
