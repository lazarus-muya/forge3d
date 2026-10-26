// SPDX-License-Identifier: Apache-2.0
#pragma once

// Terrain operations (phases.md P6.1 - P6.4).
//
//   terrain.create {name, guid, tiles, size, min_height, max_height, format, seed,
//                   frequency, octaves}
//     a terrain entity with fBm noise heights; undo removes both (redo restores the
//     same heights without regenerating them).
//
//   terrain.sculpt_stroke {terrain, tool, radius, strength, falloff, spacing, use_target,
//                          target_height, terrace_step, noise_scale, noise_seed, stamp,
//                          stamp_height, stamp_rotation, points: [[x, z, pressure], ...]}
//     one brush stroke (sculpt.hpp); points are terrain-local metres. Undo/redo restore
//     the touched tiles from a compressed diff (exact); the diff counts against the
//     history's byte cap.
//
//   terrain.erode {terrain, mode, iterations, seed, rain, evaporation, capacity,
//                  dissolving, deposition, min_slope, talus_angle, thermal_rate}
//     global hydraulic and/or thermal erosion (erosion.hpp). Reports progress through
//     ops::Context::progress, which can cancel it; undo/redo via a tile diff.
//
//   terrain.apply_rules {terrain}
//     bakes Terrain.layers' rules into the splat maps (layers.hpp); undo restores them.
//
//   terrain.set_layers {terrain, layers, source}
//     replaces Terrain.layers; with `source` (per new layer its old index, -1: new)
//     the painted weights follow reordered, added and removed layers. Undo restores
//     both. Consecutive edits without reordering merge (slider drags).
//
//   terrain.auto_material {terrain, preset}
//     replaces Terrain.layers with a ready-made set (builtin_textures.hpp:
//     grassland, alpine, desert, forest, coast) fitted to the terrain's heights and
//     bakes its rules; undo restores the layers and the splat maps.
//
//   terrain.paint_stroke {terrain, layer, erase, radius, strength, falloff, spacing,
//                         points: [[x, z, pressure], ...]}
//     paints (or erases) one layer over the rules; undo/redo via a splat tile diff.
//
// A new terrain gets default_layers() (built-in grass, rock, snow) with the rules applied.

#include "f3d/ops/registry.hpp"
#include "f3d/terrain/erosion.hpp"

#include <cstdint>
#include <vector>

#include <memory>

namespace f3d::terrain {

class StrokeRecorder;
class PaintRecorder;

void register_terrain_ops(ops::OpRegistry& registry);

/// A terrain.sculpt_stroke for a stroke the editor already applied live: executing it
/// records the recorder's diff instead of applying the points again (redo and replay
/// from the parameters give the same heights).
[[nodiscard]] std::unique_ptr<ops::Operation> make_sculpt_stroke_op(const core::Guid& terrain,
                                                                    std::unique_ptr<StrokeRecorder> applied);

/// A terrain.paint_stroke for a stroke the editor already applied live.
[[nodiscard]] std::unique_ptr<ops::Operation> make_paint_stroke_op(const core::Guid& terrain,
                                                                   std::unique_ptr<PaintRecorder> applied);

/// A terrain.erode whose result was computed elsewhere (the editor runs erosion on a
/// worker): `result` holds the eroded heights of the heightfield at `revision`. If the
/// heights changed since, executing it erodes again.
[[nodiscard]] std::unique_ptr<ops::Operation> make_erode_op(const core::Guid& terrain, const ErosionSettings& settings,
                                                            std::vector<float> result, std::uint64_t revision);

} // namespace f3d::terrain
