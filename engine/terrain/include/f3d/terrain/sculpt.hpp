// SPDX-License-Identifier: Apache-2.0
#pragma once

// Sculpt brushes (phases.md P6.2). A stroke is a list of points (terrain-local x, z in
// metres, pen pressure); DabSpacer turns them into evenly spaced dabs and each dab
// changes the heights under a circle. StrokeRecorder applies a stroke and keeps the
// tiles it touched from before the first change, so the stroke becomes one undo step
// (a TileDiff: per tile, the heights before and the bitwise change, zstd-compressed).
//
// Brushes run on the CPU: the CPU heights are the authority (raycasts, undo, saving),
// and only the dirty tiles go to the GPU. Same points + same heights = same result, bit
// for bit, so a stroke replays exactly from its parameters.

#include "f3d/terrain/terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::core::vfs {
class FileSystem;
}

namespace f3d::terrain {

enum class BrushTool : std::uint8_t { Raise, Lower, Smooth, Flatten, Terrace, Noise, Stamp, Erode };
enum class BrushFalloff : std::uint8_t { Smooth, Linear, Sphere, Constant };

[[nodiscard]] std::string_view to_string(BrushTool tool) noexcept;
[[nodiscard]] std::string_view to_string(BrushFalloff falloff) noexcept;
/// Parses "raise", "lower", ... (lower case, as the op takes them); false if unknown.
bool parse(std::string_view text, BrushTool& tool) noexcept;
bool parse(std::string_view text, BrushFalloff& falloff) noexcept;

struct Brush {
    BrushTool tool = BrushTool::Raise;
    float radius = 32.0f;  ///< metres
    float strength = 0.5f; ///< 0..1 (raise/lower/noise: strength * radius / 20 metres per dab at the centre)
    BrushFalloff falloff = BrushFalloff::Smooth;
    float spacing = 0.25f; ///< distance between dabs, as a fraction of the radius
    /// Flatten: towards target_height if use_target, else towards the height under the first dab.
    bool use_target = false;
    float target_height = 0.0f;
    float terrace_step = 8.0f; ///< metres between terraces
    float noise_scale = 16.0f; ///< metres per noise feature
    std::uint32_t noise_seed = 1;
    std::string stamp;          ///< heightmap image (project path or file path)
    float stamp_height = 16.0f; ///< metres added where the stamp is white
    float stamp_rotation = 0.0f; ///< radians
    /// Erode: hydraulic + thermal erosion iterations per dab, on the dab's neighbourhood.
    std::uint32_t erosion_iterations = 12;
};

/// Falloff weight at normalised distance `d` from the centre (0) to the rim (1).
[[nodiscard]] float falloff_weight(BrushFalloff falloff, float d) noexcept;

/// A heightmap for the stamp brush: values in [0, 1], row-major, top row first.
struct Stamp {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> values;

    /// Bilinear value at (u, v) in [0, 1] (0 outside).
    [[nodiscard]] float sample(float u, float v) const noexcept;
};

/// Loads a stamp from an image (its red channel); `files` resolves project paths and
/// may be null. Null on failure.
[[nodiscard]] std::shared_ptr<const Stamp> load_stamp(std::string_view path, const core::vfs::FileSystem* files);

struct StrokePoint {
    glm::vec2 position{0.0f}; ///< terrain-local x, z (metres)
    float pressure = 1.0f;    ///< 0..1
};

/// Turns stroke points into dabs `step` metres apart along the polyline (the first
/// point is a dab). Points can be added one at a time (live) or all at once (replay):
/// the dabs are the same.
class DabSpacer {
public:
    explicit DabSpacer(float step) noexcept : step_(step > 1e-4f ? step : 1e-4f) {}
    /// Appends the dabs completed by `point` to `dabs`.
    void add(const StrokePoint& point, std::vector<StrokePoint>& dabs);

private:
    float step_;
    bool started_ = false;
    StrokePoint previous_;
    float travelled_ = 0.0f; ///< since the last dab
};

/// The tiles a stroke changed: before, and after XOR before, zstd-compressed (a tile
/// block is its 257 x 257 samples, shared edges included).
struct TileDiff {
    struct Tile {
        std::uint32_t index = 0; ///< tz * tiles + tx
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> delta;
    };
    std::vector<Tile> tiles;

    /// Writes the before (undo) or after (redo) heights back; marks the tiles dirty and
    /// updates the bounds.
    void apply_before(Heightfield& heightfield) const;
    void apply_after(Heightfield& heightfield) const;
    [[nodiscard]] std::size_t bytes() const noexcept;
};

/// The diff between `before` (a copy of the heights) and `after` (same size) for the
/// tiles that differ.
[[nodiscard]] TileDiff diff_tiles(const Heightfield& layout, const std::vector<float>& before,
                                  const std::vector<float>& after);

/// Applies a stroke to a heightfield as its points arrive and records what it changed.
class StrokeRecorder {
public:
    StrokeRecorder(std::shared_ptr<Heightfield> heightfield, const Terrain& terrain, Brush brush,
                   std::shared_ptr<const Stamp> stamp = nullptr);

    /// Adds a point: applies the dabs it completes (and marks their tiles dirty).
    void add(const StrokePoint& point);
    /// The points so far (the op's parameters).
    [[nodiscard]] const std::vector<StrokePoint>& points() const noexcept { return points_; }
    [[nodiscard]] std::size_t dab_count() const noexcept { return dabs_applied_; }
    [[nodiscard]] const Brush& brush() const noexcept { return brush_; }
    [[nodiscard]] const std::shared_ptr<Heightfield>& heightfield() const noexcept { return heightfield_; }

    /// Ends the stroke: the compressed diff of every touched tile.
    [[nodiscard]] TileDiff finish() const;
    /// Puts the touched tiles back as they were before the stroke (cancel).
    void restore();

private:
    void apply_dab(const StrokePoint& dab);
    void capture_tiles(std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1);

    std::shared_ptr<Heightfield> heightfield_;
    Terrain terrain_;
    Brush brush_;
    std::shared_ptr<const Stamp> stamp_;
    DabSpacer spacer_;
    std::vector<StrokePoint> points_;
    std::vector<StrokePoint> dabs_; ///< scratch
    std::size_t dabs_applied_ = 0;
    bool have_flatten_height_ = false;
    float flatten_height_ = 0.0f;
    std::vector<std::int32_t> slot_of_tile_;     ///< -1: not captured
    std::vector<std::uint32_t> captured_;        ///< tile indices, in capture order
    std::vector<std::vector<float>> before_;     ///< per captured tile, 257 x 257
    std::vector<float> scratch_;                 ///< smoothing
};

} // namespace f3d::terrain
