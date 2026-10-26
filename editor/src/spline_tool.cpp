// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/spline_tool.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"

#include <nlohmann/json.hpp>

#include <cmath>

namespace f3d::editor {
namespace {

/// The ground under a ray: a terrain, else the plane y = 0.
std::optional<std::pair<glm::vec3, core::Guid>> ground(scene::World& world, const glm::vec3& origin,
                                                       const glm::vec3& direction) {
    if (const auto hit = terrain::raycast(world, origin, direction)) {
        return std::pair{hit->point, world.guid(hit->entity)};
    }
    if (direction.y < -1e-4f) {
        return std::pair{origin + direction * (-origin.y / direction.y), core::Guid{}};
    }
    return std::nullopt;
}

/// Distance from `p` to the ray (origin, unit direction), and along it.
std::pair<float, float> ray_distance(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& p) {
    const float along = glm::dot(p - origin, direction);
    return {glm::length(p - (origin + direction * along)), along};
}

} // namespace

core::Guid SplineTool::current_spline() const {
    const core::Guid active = world_.selection().active();
    const entt::entity e = active.is_null() ? entt::null : world_.find(active);
    return e != entt::null && world_.registry().all_of<world::Spline>(e) ? active : core::Guid{};
}

void SplineTool::start_new(world::SplineKind kind) {
    new_kind = kind;
    selected_ = -1;
    scene_editor_.set_selection({});
}

bool SplineTool::set_points(const core::Guid& spline, const std::vector<glm::vec3>& points, bool merge) {
    nlohmann::json value = nlohmann::json::array();
    for (const glm::vec3& p : points) {
        value.push_back({p.x, p.y, p.z});
    }
    return scene_editor_.run("entity.set_property",
                             {{"entity", spline.to_string()}, {"path", "Spline.points"}, {"value", value}}, merge);
}

void SplineTool::update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float) {
    const auto hit = ground(world_, origin, direction);
    cursor_.reset();
    if (hit) {
        cursor_ = hit->first;
    }
    const core::Guid guid = current_spline();
    const entt::entity e = guid.is_null() ? entt::null : world_.find(guid);
    std::vector<glm::vec3> points = e == entt::null ? std::vector<glm::vec3>{} : world::spline_world_points(world_, e);
    if (selected_ >= static_cast<int>(points.size())) {
        selected_ = -1;
    }

    // The point under the cursor: close to the ray for its distance.
    hover_ = -1;
    float best = 1e30f;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto [off, along] = ray_distance(origin, direction, points[i]);
        if (along > 0.0f && off < std::max(0.5f, along * 0.015f) && off < best) {
            best = off;
            hover_ = static_cast<int>(i);
        }
    }

    const bool press = pressed && !was_pressed_;
    was_pressed_ = pressed;
    if (press) {
        if (hover_ >= 0) {
            selected_ = hover_;
            dragging_ = true;
            drag_moved_ = false;
        } else if (hit) {
            if (e == entt::null) {
                // A new spline at the click.
                nlohmann::json params = {{"kind", std::string(world::to_string(new_kind))},
                                         {"points", {{hit->first.x, hit->first.y, hit->first.z}}}};
                if (!hit->second.is_null()) {
                    params["terrain"] = hit->second.to_string();
                }
                const core::Guid created = core::Guid::generate();
                params["guid"] = created.to_string();
                if (scene_editor_.run("spline.create", params)) {
                    scene_editor_.set_selection({created}, created);
                    selected_ = 0;
                }
            } else {
                // Near the curve: between the points around it; else after the last.
                const world::Spline& spline = world_.registry().get<world::Spline>(e);
                std::size_t insert = points.size();
                if (points.size() >= 2) {
                    const auto samples = world::evaluate_spline(points, spline.closed, 1.0f);
                    const world::SplineShape shape(samples, std::max(spline.width * 0.5f, 1.0f));
                    if (const auto near = shape.nearest(hit->first.x, hit->first.z)) {
                        // Each control point's distance along the curve (its nearest sample;
                        // the samples include every control point), then the first after the click.
                        std::vector<float> point_distance(points.size(), 0.0f);
                        std::size_t sample = 0;
                        for (std::size_t i = 0; i < points.size(); ++i) {
                            float closest = 1e30f;
                            for (std::size_t j = sample; j < samples.size(); ++j) {
                                const float d = glm::length(samples[j].position - points[i]);
                                if (d < closest) {
                                    closest = d;
                                    sample = j;
                                }
                                if (d < 1e-3f) {
                                    break;
                                }
                            }
                            point_distance[i] = samples[sample].distance;
                        }
                        std::size_t k = 1;
                        while (k < points.size() && point_distance[k] < near->along) {
                            ++k;
                        }
                        insert = k;
                    }
                }
                const glm::vec3 base(world_.transform(e).world[3]);
                std::vector<glm::vec3> local = spline.points;
                local.insert(local.begin() + static_cast<std::ptrdiff_t>(insert), hit->first - base);
                if (set_points(guid, local, false)) {
                    selected_ = static_cast<int>(insert);
                }
            }
        }
    }
    if (dragging_) {
        if (!pressed) {
            dragging_ = false;
        } else if (hit && e != entt::null && selected_ >= 0) {
            const glm::vec3 base(world_.transform(e).world[3]);
            std::vector<glm::vec3> local = world_.registry().get<world::Spline>(e).points;
            const glm::vec3 moved = hit->first - base;
            if (glm::length(local[static_cast<std::size_t>(selected_)] - moved) > 1e-3f) {
                local[static_cast<std::size_t>(selected_)] = moved;
                set_points(guid, local, drag_moved_); // the whole drag is one undo step
                drag_moved_ = true;
            }
        }
    }
}

void SplineTool::leave() {
    dragging_ = false;
    was_pressed_ = false;
    cursor_.reset();
    hover_ = -1;
}

void SplineTool::cancel() {
    if (dragging_) {
        dragging_ = false;
        if (drag_moved_) {
            scene_editor_.stack().undo(); // put the point back
        }
        return;
    }
    selected_ = -1;
}

bool SplineTool::erase() {
    const core::Guid guid = current_spline();
    if (guid.is_null() || selected_ < 0) {
        return false;
    }
    const entt::entity e = world_.find(guid);
    std::vector<glm::vec3> local = world_.registry().get<world::Spline>(e).points;
    if (local.size() <= 1) {
        scene_editor_.run("entity.delete", {{"entity", guid.to_string()}});
        selected_ = -1;
        return true;
    }
    local.erase(local.begin() + selected_);
    set_points(guid, local, false);
    selected_ = std::min(selected_, static_cast<int>(local.size()) - 1);
    return true;
}

void SplineTool::draw() const {
    const core::Guid guid = current_spline();
    const entt::entity e = guid.is_null() ? entt::null : world_.find(guid);
    if (e != entt::null) {
        const world::Spline& spline = world_.registry().get<world::Spline>(e);
        const std::vector<glm::vec3> points = world::spline_world_points(world_, e);
        const auto samples = world::evaluate_spline(points, spline.closed, 2.0f);
        const glm::vec3 lift(0.0f, 0.3f, 0.0f);
        for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
            core::debug::line(samples[i].position + lift, samples[i + 1].position + lift, core::debug::rgba8(255, 200, 60));
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            const bool sel = static_cast<int>(i) == selected_;
            const bool hov = static_cast<int>(i) == hover_;
            const std::uint32_t color = sel ? core::debug::rgba8(255, 90, 60)
                                        : hov ? core::debug::rgba8(255, 255, 255)
                                              : core::debug::rgba8(255, 220, 120);
            const float r = std::max(0.4f, spline.width * 0.12f) * (sel || hov ? 1.5f : 1.0f);
            const glm::vec3 p = points[i] + lift;
            core::debug::line(p - glm::vec3(r, 0, 0), p + glm::vec3(r, 0, 0), color);
            core::debug::line(p - glm::vec3(0, 0, r), p + glm::vec3(0, 0, r), color);
            core::debug::line(p, p + glm::vec3(0, r * 3.0f, 0), color);
        }
    }
    if (cursor_ && hover_ < 0) {
        draw_brush_ring(world_, *cursor_, 0.75f, core::debug::rgba8(255, 220, 120));
    }
}

} // namespace f3d::editor
