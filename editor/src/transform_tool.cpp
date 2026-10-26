// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/transform_tool.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace f3d::editor {

std::string_view to_string(GizmoOperation op) noexcept {
    switch (op) {
    case GizmoOperation::Translate:
        return "Move";
    case GizmoOperation::Rotate:
        return "Rotate";
    case GizmoOperation::Scale:
        return "Scale";
    }
    return "?";
}

std::string_view to_string(GizmoSpace space) noexcept {
    return space == GizmoSpace::World ? "World" : "Local";
}

std::string_view to_string(PivotMode mode) noexcept {
    switch (mode) {
    case PivotMode::Median:
        return "Median Point";
    case PivotMode::IndividualOrigins:
        return "Individual Origins";
    case PivotMode::ActiveElement:
        return "Active Element";
    case PivotMode::Cursor:
        return "3D Cursor";
    }
    return "?";
}

std::string_view to_string(SnapTarget target) noexcept {
    switch (target) {
    case SnapTarget::None:
        return "No surface snap";
    case SnapTarget::Surface:
        return "Snap to surface";
    case SnapTarget::Vertex:
        return "Snap to vertex";
    }
    return "?";
}

TransformTool::TransformTool(scene::World& world, ops::OpStack& stack, const ops::OpRegistry& registry)
    : world_(world), stack_(stack), registry_(registry) {}

std::vector<core::Guid> TransformTool::targets() const {
    std::vector<entt::entity> selected;
    for (const core::Guid& guid : world_.selection().entities()) {
        if (const entt::entity e = world_.find(guid); e != entt::null) {
            selected.push_back(e);
        }
    }
    std::vector<core::Guid> out;
    for (const entt::entity e : selected) {
        bool ancestor_selected = false;
        for (entt::entity p = world_.parent(e); p != entt::null && !ancestor_selected; p = world_.parent(p)) {
            ancestor_selected = std::find(selected.begin(), selected.end(), p) != selected.end();
        }
        if (!ancestor_selected) {
            out.push_back(world_.guid(e));
        }
    }
    return out;
}

glm::vec3 TransformTool::pivot_point(const std::vector<core::Guid>& guids) const {
    const auto origin = [&](const core::Guid& guid) { return glm::vec3(world_.world_matrix(world_.find(guid))[3]); };
    switch (pivot) {
    case PivotMode::Cursor:
        return cursor;
    case PivotMode::ActiveElement: {
        const core::Guid active = world_.selection().active();
        if (!active.is_null() && world_.find(active) != entt::null) {
            return origin(active);
        }
        break; // no active entity: fall back to the median
    }
    case PivotMode::Median:
    case PivotMode::IndividualOrigins:
        break;
    }
    glm::vec3 sum{0.0f};
    for (const core::Guid& guid : guids) {
        sum += origin(guid);
    }
    return guids.empty() ? glm::vec3{0.0f} : sum / static_cast<float>(guids.size());
}

std::optional<glm::mat4> TransformTool::gizmo_matrix() const {
    const std::vector<core::Guid> guids = targets();
    if (guids.empty()) {
        return std::nullopt;
    }
    glm::mat4 m(1.0f);
    if (space == GizmoSpace::Local) {
        core::Guid oriented = world_.selection().active();
        if (oriented.is_null() || world_.find(oriented) == entt::null) {
            oriented = guids.front();
        }
        glm::vec3 t, s;
        glm::quat r;
        scene::decompose(world_.world_matrix(world_.find(oriented)), t, r, s);
        m = glm::mat4_cast(r);
    }
    m[3] = glm::vec4(pivot_point(guids), 1.0f);
    return m;
}

bool TransformTool::begin() {
    if (active_) {
        return true;
    }
    const std::optional<glm::mat4> gizmo = gizmo_matrix();
    if (!gizmo) {
        return false;
    }
    start_gizmo_ = *gizmo;
    drag_operation_ = operation;
    drag_targets_.clear();
    for (const core::Guid& guid : targets()) {
        const entt::entity e = world_.find(guid);
        const entt::entity parent = world_.parent(e);
        drag_targets_.push_back(
            {guid, world_.world_matrix(e), parent == entt::null ? glm::mat4(1.0f) : world_.world_matrix(parent)});
    }
    std::string name(to_string(drag_operation_));
    if (drag_targets_.size() > 1) {
        name += " " + std::to_string(drag_targets_.size()) + " entities";
    }
    stack_.begin_transaction(std::move(name));
    active_ = true;
    return true;
}

void TransformTool::set_property(const core::Guid& entity, const char* path, const glm::vec3& value) {
    nlohmann::json params = {{"entity", entity.to_string()}, {"path", path}, {"value", {value.x, value.y, value.z}}};
    // Inside the drag's transaction, merge folds this frame into the entity's earlier edit.
    stack_.execute(registry_.create("entity.set_property", params), true);
}

void TransformTool::set_property(const core::Guid& entity, const char* path, const glm::quat& value) {
    nlohmann::json params = {
        {"entity", entity.to_string()}, {"path", path}, {"value", {value.w, value.x, value.y, value.z}}};
    stack_.execute(registry_.create("entity.set_property", params), true);
}

void TransformTool::update(const glm::mat4& gizmo) {
    if (!active_) {
        return;
    }
    F3D_ZONE;
    const glm::mat4 delta = gizmo * glm::inverse(start_gizmo_);
    // The linear part of the drag and how far the pivot moved.
    const glm::mat3 linear = glm::mat3(gizmo) * glm::inverse(glm::mat3(start_gizmo_));
    const glm::vec3 pivot_move = glm::vec3(gizmo[3]) - glm::vec3(start_gizmo_[3]);

    for (const Target& target : drag_targets_) {
        glm::mat4 world;
        if (pivot == PivotMode::IndividualOrigins) {
            // Rotate/scale each entity about its own origin; translation moves them all alike.
            const glm::vec3 origin = glm::vec3(target.start_world[3]);
            glm::mat4 about = glm::mat4(linear);
            world = glm::translate(glm::mat4(1.0f), origin + pivot_move) * about *
                    glm::translate(glm::mat4(1.0f), -origin) * target.start_world;
        } else {
            world = delta * target.start_world;
        }
        glm::vec3 translation, scale;
        glm::quat rotation;
        scene::decompose(glm::inverse(target.parent_world) * world, translation, rotation, scale);

        set_property(target.guid, "Transform.translation", translation);
        if (drag_operation_ == GizmoOperation::Rotate) {
            set_property(target.guid, "Transform.rotation", rotation);
        } else if (drag_operation_ == GizmoOperation::Scale) {
            set_property(target.guid, "Transform.scale", scale);
        }
    }
}

void TransformTool::end() {
    if (!active_) {
        return;
    }
    stack_.end_transaction();
    active_ = false;
    drag_targets_.clear();
}

void TransformTool::cancel() {
    if (!active_) {
        return;
    }
    stack_.abort_transaction();
    active_ = false;
    drag_targets_.clear();
}

} // namespace f3d::editor
