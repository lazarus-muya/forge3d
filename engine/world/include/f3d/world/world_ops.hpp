// SPDX-License-Identifier: Apache-2.0
#pragma once

// World-building operations (phases.md P7.1 - P7.4).
//
//   scatter.paint_stroke {scatter, erase, radius, strength, falloff, spacing,
//                         points: [[x, z, pressure], ...]}
//     paints (or erases) density into a scatter's mask along a stroke; points are world
//     x/z metres. The first stroke creates the mask (full, or empty for paint_only
//     scatters). Undo/redo restore the touched cells exactly from a compressed diff.
//
//   spline.create {name, guid, kind, terrain, points: [[x, y, z], ...], paint_surface}
//     a Spline entity (road, path, river, fence, custom) with the kind's settings, at its
//     first point; paint_surface ("auto": dirt under roads and paths, gravel under
//     rivers) adds that built-in layer to the terrain if it has none of that name.
//
//   water.create {name, guid, kind, terrain, position}
//     a WaterBody entity (lake or ocean) at `position`; its height is the water level.
//
// Splines and water bodies are then edited with entity.set_property; what they do to
// the landscape (roads, riverbeds, lake surfaces) is derived from them.

#include "f3d/ops/registry.hpp"

#include <memory>

namespace f3d::world {

class ScatterPaintRecorder;

/// Registers the world types (Scatter, WaterBody, Spline) and operations. Idempotent per registry.
void register_world_ops(ops::OpRegistry& registry);

/// A scatter.paint_stroke for a stroke the editor already applied live.
[[nodiscard]] std::unique_ptr<ops::Operation> make_scatter_paint_op(const core::Guid& scatter,
                                                                    std::unique_ptr<ScatterPaintRecorder> applied,
                                                                    bool created_mask);

} // namespace f3d::world
