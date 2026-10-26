// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/paint_tool.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/layers.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"

namespace f3d::editor {

PaintTool::~PaintTool() = default;

void PaintTool::update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) {
    const std::optional<terrain::TerrainHit> hit = terrain::raycast(world_, origin, direction);
    cursor_.reset();
    if (hit) {
        cursor_ = hit->point;
        cursor_terrain_ = world_.guid(hit->entity);
    }
    if (stroke_) {
        // The terrain went away mid-stroke (an undo, a script): drop the stroke.
        const entt::entity e = world_.find(terrain_);
        if (e == entt::null || terrain::heightfield_of(world_, e) != stroke_->heightfield()) {
            stroke_.reset();
        }
    }
    if (!pressed) {
        if (stroke_) {
            end();
        }
        return;
    }
    if (!hit) {
        return;
    }
    if (!stroke_) {
        const entt::entity e = hit->entity;
        const terrain::Terrain* t = world_.registry().try_get<terrain::Terrain>(e);
        std::shared_ptr<terrain::Heightfield> hf = terrain::heightfield_of(world_, e);
        if (!t || !hf || layer >= terrain::kMaxLayers) {
            return;
        }
        terrain_ = cursor_terrain_;
        terrain_origin_ = terrain::terrain_origin(world_, e);
        stroke_ = std::make_unique<terrain::PaintRecorder>(std::move(hf), *t, brush, layer, erase_layer);
    } else if (cursor_terrain_ != terrain_) {
        return; // over another terrain
    }
    const glm::vec3 local = *cursor_ - terrain_origin_;
    stroke_->add({{local.x, local.z}, pressure});
}

void PaintTool::leave() {
    if (stroke_) {
        end();
    }
    cursor_.reset();
}

void PaintTool::cancel() {
    if (stroke_) {
        stroke_->restore();
        stroke_.reset();
    }
}

void PaintTool::end() {
    if (stack_.execute(terrain::make_paint_stroke_op(terrain_, std::move(stroke_)))) {
        ++strokes_;
    }
    stroke_.reset();
}

void PaintTool::draw() const {
    if (!cursor_) {
        return;
    }
    const std::uint32_t color = erase_layer ? core::debug::rgba8(255, 120, 110)
                                            : (stroke_ ? core::debug::rgba8(120, 220, 255) : core::debug::rgba8(235, 235, 235));
    draw_brush_ring(world_, *cursor_, brush.radius, color);
}

} // namespace f3d::editor
