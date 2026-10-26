// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/viewport_tool.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"

#include <cmath>

namespace f3d::editor {

ViewportTool* LandscapeTools::sync() {
    ViewportTool* newest = nullptr;
    for (ViewportTool* tool : tools_) {
        if (tool->enabled && tool != active_) {
            newest = tool;
        }
    }
    if (newest) {
        activate(newest);
    } else if (active_ && !active_->enabled) {
        if (active_->busy()) {
            active_->leave();
        }
        active_ = nullptr;
    }
    return active_;
}

void LandscapeTools::activate(ViewportTool* tool) {
    for (ViewportTool* other : tools_) {
        if (other != tool && other->enabled) {
            if (other->busy()) {
                other->leave(); // commits what is in progress
            }
            other->enabled = false;
        }
    }
    if (tool) {
        tool->enabled = true;
    }
    active_ = tool;
}

void LandscapeTools::toggle(ViewportTool* tool) {
    activate(tool && tool == active_ && tool->enabled ? nullptr : tool);
}

void draw_brush_ring(scene::World& world, const glm::vec3& centre, float radius, std::uint32_t color) {
    // The terrain under the centre: a downward ray from well above it.
    const std::optional<terrain::TerrainHit> hit =
        terrain::raycast(world, centre + glm::vec3(0.0f, 1.0e4f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    const std::shared_ptr<terrain::Heightfield> hf = hit ? terrain::surface_of(world, hit->entity) : nullptr;
    if (!hf) {
        return;
    }
    const glm::vec3 origin = terrain::terrain_origin(world, hit->entity);
    const float lift = 0.002f * radius + 0.02f; // above the surface, against z-fighting
    const auto draped = [&](float r, float angle) {
        glm::vec3 p = centre + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * r;
        p.y = hf->height_at(p.x - origin.x, p.z - origin.z) + origin.y + lift;
        return p;
    };
    constexpr int kSegments = 64;
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = 6.2831853f * static_cast<float>(i) / kSegments;
        const float a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
        core::debug::line(draped(radius, a0), draped(radius, a1), color);
        if (i % 2 == 0) { // the falloff's half-strength ring, dashed
            core::debug::line(draped(radius * 0.5f, a0), draped(radius * 0.5f, a1), color);
        }
    }
    const glm::vec3 up(centre.x, hit->point.y + lift, centre.z);
    core::debug::line(up, up + glm::vec3(0.0f, radius * 0.15f, 0.0f), color);
}

} // namespace f3d::editor
