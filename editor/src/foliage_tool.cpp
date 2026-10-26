// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/foliage_tool.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/core/log.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/world_ops.hpp"

namespace f3d::editor {

FoliageTool::~FoliageTool() = default;

core::Guid FoliageTool::current_scatter() const {
    const auto is_scatter = [&](const core::Guid& guid) {
        const entt::entity e = guid.is_null() ? entt::null : world_.find(guid);
        return e != entt::null && world_.registry().all_of<world::Scatter>(e);
    };
    if (is_scatter(target)) {
        return target;
    }
    const core::Guid active = world_.selection().active();
    return is_scatter(active) ? active : core::Guid{};
}

void FoliageTool::update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) {
    cursor_.reset();
    if (const auto hit = terrain::raycast(world_, origin, direction)) {
        cursor_ = hit->point;
    } else if (direction.y < -1e-4f) { // flat ground at y = 0
        cursor_ = origin + direction * (-origin.y / direction.y);
    }
    if (stroke_) {
        const entt::entity e = world_.find(scatter_);
        if (e == entt::null || world::scatter_mask_of(world_, e) != stroke_->mask()) {
            stroke_.reset(); // the scatter or its mask went away mid-stroke (an undo, a script)
        }
    }
    if (!pressed) {
        if (stroke_) {
            end();
        }
        return;
    }
    if (!cursor_) {
        return;
    }
    if (!stroke_) {
        const core::Guid guid = current_scatter();
        if (guid.is_null()) {
            return;
        }
        const entt::entity e = world_.find(guid);
        const world::Scatter& scatter = world_.registry().get<world::Scatter>(e);
        std::shared_ptr<world::ScatterMask> mask = world::scatter_mask_of(world_, e);
        created_mask_ = !mask;
        if (!mask) {
            mask = world::new_scatter_mask(scatter);
            world::scatter_masks(world_).set(guid, mask);
        }
        world_.update_transforms();
        scatter_ = guid;
        stroke_ = std::make_unique<world::ScatterPaintRecorder>(mask, glm::vec3(world_.transform(e).world[3]),
                                                                scatter.area, brush, erase_instances);
    }
    stroke_->add({{cursor_->x, cursor_->z}, pressure});
}

void FoliageTool::leave() {
    if (stroke_) {
        end();
    }
    cursor_.reset();
}

void FoliageTool::cancel() {
    if (stroke_) {
        stroke_->restore();
        if (created_mask_) {
            world::scatter_masks(world_).erase(scatter_);
        }
        stroke_.reset();
    }
}

void FoliageTool::end() {
    if (stack_.execute(world::make_scatter_paint_op(scatter_, std::move(stroke_), created_mask_))) {
        ++strokes_;
    }
    stroke_.reset();
}

void FoliageTool::draw() const {
    if (!cursor_) {
        return;
    }
    const bool usable = !current_scatter().is_null();
    const std::uint32_t color = !usable           ? core::debug::rgba8(150, 150, 150)
                                : erase_instances ? core::debug::rgba8(255, 120, 110)
                                : stroke_         ? core::debug::rgba8(120, 255, 140)
                                                  : core::debug::rgba8(200, 245, 200);
    draw_brush_ring(world_, *cursor_, brush.radius, color);
}

} // namespace f3d::editor
