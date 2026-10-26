// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/sculpt_tool.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/core/log.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

namespace f3d::editor {

SculptTool::~SculptTool() = default;

void SculptTool::update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) {
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
        stamp_failed_ = false;
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
        if (!t || !hf || stamp_failed_) {
            return;
        }
        std::shared_ptr<const terrain::Stamp> stamp;
        if (brush.tool == terrain::BrushTool::Stamp) {
            stamp = terrain::load_stamp(brush.stamp, files_);
            if (!stamp) {
                F3D_LOG_WARN("Sculpt: cannot read the stamp '{}'", brush.stamp);
                stamp_failed_ = true; // once per press
                return;
            }
        }
        terrain_ = cursor_terrain_;
        terrain_origin_ = terrain::terrain_origin(world_, e);
        stroke_ = std::make_unique<terrain::StrokeRecorder>(std::move(hf), *t, brush, std::move(stamp));
    } else if (cursor_terrain_ != terrain_) {
        return; // over another terrain
    }
    const glm::vec3 local = *cursor_ - terrain_origin_;
    stroke_->add({{local.x, local.z}, pressure});
}

void SculptTool::leave() {
    if (stroke_) {
        end();
    }
    cursor_.reset();
}

void SculptTool::cancel() {
    if (stroke_) {
        stroke_->restore();
        stroke_.reset();
    }
}

void SculptTool::end() {
    if (stack_.execute(terrain::make_sculpt_stroke_op(terrain_, std::move(stroke_)))) {
        ++strokes_;
    }
    stroke_.reset();
}

void SculptTool::draw() const {
    if (!cursor_) {
        return;
    }
    const entt::entity e = world_.find(cursor_terrain_);
    const std::shared_ptr<terrain::Heightfield> hf = e == entt::null ? nullptr : terrain::heightfield_of(world_, e);
    if (!hf) {
        return;
    }
    const glm::vec3 origin = terrain::terrain_origin(world_, e);
    const float lift = 0.002f * brush.radius + 0.02f; // above the surface, against z-fighting
    const auto draped = [&](float radius, float angle) {
        glm::vec3 p = *cursor_ + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * radius;
        p.y = hf->height_at(p.x - origin.x, p.z - origin.z) + origin.y + lift;
        return p;
    };
    constexpr int kSegments = 64;
    const std::uint32_t color = stroke_ ? core::debug::rgba8(255, 220, 90) : core::debug::rgba8(235, 235, 235);
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = 6.2831853f * static_cast<float>(i) / kSegments;
        const float a1 = 6.2831853f * static_cast<float>(i + 1) / kSegments;
        core::debug::line(draped(brush.radius, a0), draped(brush.radius, a1), color);
        if (i % 2 == 0) { // the falloff's half-strength ring, dashed
            core::debug::line(draped(brush.radius * 0.5f, a0), draped(brush.radius * 0.5f, a1), color);
        }
    }
    const glm::vec3 up = *cursor_ + glm::vec3(0.0f, lift, 0.0f);
    core::debug::line(up, up + glm::vec3(0.0f, brush.radius * 0.15f, 0.0f), color);
}

bool SculptTool::apply_json(const nlohmann::json& settings, std::string* error) {
    if (!settings.is_object()) {
        if (error) {
            *error = "brush settings must be an object";
        }
        return false;
    }
    terrain::Brush b = brush;
    try {
        if (settings.contains("tool") && !terrain::parse(settings.at("tool").get<std::string>(), b.tool)) {
            throw std::runtime_error("unknown tool '" + settings.at("tool").get<std::string>() + "'");
        }
        if (settings.contains("falloff") && !terrain::parse(settings.at("falloff").get<std::string>(), b.falloff)) {
            throw std::runtime_error("unknown falloff '" + settings.at("falloff").get<std::string>() + "'");
        }
        b.radius = settings.value("radius", b.radius);
        b.strength = settings.value("strength", b.strength);
        b.spacing = settings.value("spacing", b.spacing);
        b.use_target = settings.value("use_target", b.use_target);
        b.target_height = settings.value("target_height", b.target_height);
        b.terrace_step = settings.value("terrace_step", b.terrace_step);
        b.noise_scale = settings.value("noise_scale", b.noise_scale);
        b.noise_seed = settings.value("noise_seed", b.noise_seed);
        b.stamp = settings.value("stamp", b.stamp);
        b.stamp_height = settings.value("stamp_height", b.stamp_height);
        b.stamp_rotation = settings.value("stamp_rotation", b.stamp_rotation);
        b.erosion_iterations = settings.value("erosion_iterations", b.erosion_iterations);
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
    brush = std::move(b);
    return true;
}

} // namespace f3d::editor
