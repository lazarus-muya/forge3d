// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/water_tool.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace f3d::editor {
namespace {

constexpr std::uint32_t kPreviewCells = 96; ///< per side: coarse, so hovering stays cheap
constexpr std::size_t kMaxPreviewEdges = 4096;

} // namespace

void WaterTool::update_preview() {
    if (!cursor_) {
        preview_ = {};
        return;
    }
    const entt::entity t = world_.find(cursor_terrain_);
    const auto heights = terrain::surface_of(world_, t);
    if (!heights) {
        preview_ = {};
        return;
    }
    // Redo only when the cursor moved more than a preview cell, or the depth or ground changed.
    const float cell = std::max(heights->size() / static_cast<float>(kPreviewCells), heights->spacing());
    if (preview_depth_ == fill_depth && preview_revision_ == heights->revision() &&
        glm::length(*cursor_ - preview_at_) < cell * 0.5f) {
        return;
    }
    preview_at_ = *cursor_;
    preview_depth_ = fill_depth;
    preview_revision_ = heights->revision();
    preview_origin_ = terrain::terrain_origin(world_, t);
    const world::WaterBody defaults;
    preview_ =
        world::flood_basin(*heights, glm::vec2(preview_at_.x - preview_origin_.x, preview_at_.z - preview_origin_.z),
                           preview_at_.y - preview_origin_.y + fill_depth, defaults.extent, kPreviewCells);
}

core::Guid WaterTool::place_lake(const glm::vec3& ground, const core::Guid& terrain) {
    const core::Guid created = core::Guid::generate();
    nlohmann::json params = {
        {"kind", "lake"}, {"guid", created.to_string()}, {"position", {ground.x, ground.y + fill_depth, ground.z}}};
    if (!terrain.is_null()) {
        params["terrain"] = terrain.to_string();
    }
    if (!scene_editor_.run("water.create", params)) {
        return {};
    }
    scene_editor_.set_selection({created}, created);
    return created;
}

void WaterTool::update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float) {
    cursor_.reset();
    cursor_terrain_ = {};
    if (const auto hit = terrain::raycast(world_, origin, direction)) { // lakes need a terrain to fill
        cursor_ = hit->point;
        cursor_terrain_ = world_.guid(hit->entity);
    }
    const bool press = pressed && !was_pressed_;
    was_pressed_ = pressed;
    if (press && cursor_) {
        place_lake(*cursor_, cursor_terrain_);
    }
    update_preview();
}

void WaterTool::leave() {
    was_pressed_ = false;
    cursor_.reset();
    preview_ = {};
}

void WaterTool::cancel() {} // a click is instant: nothing is ever in progress

void WaterTool::draw() const {
    if (!cursor_) {
        return;
    }
    const std::uint32_t color = core::debug::rgba8(90, 190, 255);
    draw_brush_ring(world_, *cursor_, 2.0f, color);
    const glm::vec3 level = *cursor_ + glm::vec3(0.0f, fill_depth, 0.0f);
    core::debug::line(*cursor_, level, color);
    constexpr float r = 1.5f;
    core::debug::line(level - glm::vec3(r, 0, 0), level + glm::vec3(r, 0, 0), color);
    core::debug::line(level - glm::vec3(0, 0, r), level + glm::vec3(0, 0, r), color);

    // The shore: cell edges between wet and dry, at the level.
    const world::FloodResult& f = preview_;
    if (f.cells == 0) {
        return;
    }
    const auto wet = [&](std::int64_t x, std::int64_t z) {
        return x >= 0 && z >= 0 && x < f.width && z < f.height &&
               f.wet[static_cast<std::size_t>(z) * f.width + static_cast<std::size_t>(x)] != 0;
    };
    const float y = level.y + 0.05f;
    const auto corner = [&](std::int64_t x, std::int64_t z) {
        return glm::vec3(preview_origin_.x + f.origin.x + static_cast<float>(x) * f.step, y,
                         preview_origin_.z + f.origin.y + static_cast<float>(z) * f.step);
    };
    std::size_t edges = 0;
    for (std::int64_t z = 0; z < f.height && edges < kMaxPreviewEdges; ++z) {
        for (std::int64_t x = 0; x < f.width; ++x) {
            if (!wet(x, z)) {
                continue;
            }
            if (!wet(x - 1, z)) {
                core::debug::line(corner(x, z), corner(x, z + 1), color);
                ++edges;
            }
            if (!wet(x + 1, z)) {
                core::debug::line(corner(x + 1, z), corner(x + 1, z + 1), color);
                ++edges;
            }
            if (!wet(x, z - 1)) {
                core::debug::line(corner(x, z), corner(x + 1, z), color);
                ++edges;
            }
            if (!wet(x, z + 1)) {
                core::debug::line(corner(x, z + 1), corner(x + 1, z + 1), color);
                ++edges;
            }
        }
    }
}

} // namespace f3d::editor
