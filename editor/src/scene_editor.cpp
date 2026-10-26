// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/scene_editor.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/reflect_json.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>

namespace f3d::editor {
namespace {

nlohmann::json guid_list(const std::vector<core::Guid>& guids) {
    nlohmann::json list = nlohmann::json::array();
    for (const core::Guid& guid : guids) {
        list.push_back(guid.to_string());
    }
    return list;
}

/// A guid as op JSON (null for the null guid).
nlohmann::json guid_json(const core::Guid& guid) {
    return guid.is_null() ? nlohmann::json() : nlohmann::json(guid.to_string());
}

} // namespace

SceneEditor::SceneEditor(scene::World& world, ops::OpStack& stack, const ops::OpRegistry& registry)
    : world_(world), stack_(stack), registry_(registry) {}

bool SceneEditor::run(std::string_view id, const nlohmann::json& params, bool merge) {
    std::unique_ptr<ops::Operation> op;
    try {
        op = registry_.create(id, params);
    } catch (const std::exception& e) {
        F3D_LOG_ERROR("{}: bad parameters: {}", id, e.what());
        return false;
    }
    if (!op) {
        F3D_LOG_ERROR("Unknown operation {}", id);
        return false;
    }
    return stack_.execute(std::move(op), merge);
}

void SceneEditor::set_selection(std::vector<core::Guid> entities, core::Guid active) {
    const scene::Selection& current = world_.selection();
    if (entities == current.entities() && (active.is_null() || active == current.active())) {
        return;
    }
    run("selection.set", {{"entities", guid_list(entities)}, {"active", guid_json(active)}});
}

void SceneEditor::select_all() {
    std::vector<core::Guid> all;
    for (const entt::entity e : world_.depth_first()) {
        all.push_back(world_.guid(e));
    }
    set_selection(std::move(all), world_.selection().active());
}

std::vector<core::Guid> SceneEditor::selected_roots() const {
    std::vector<entt::entity> selected;
    for (const core::Guid& guid : world_.selection().entities()) {
        if (const entt::entity e = world_.find(guid); e != entt::null) {
            selected.push_back(e);
        }
    }
    std::vector<core::Guid> out;
    for (const entt::entity e : selected) {
        bool covered = false;
        for (entt::entity p = world_.parent(e); p != entt::null && !covered; p = world_.parent(p)) {
            covered = std::find(selected.begin(), selected.end(), p) != selected.end();
        }
        if (!covered) {
            out.push_back(world_.guid(e));
        }
    }
    return out;
}

core::Guid SceneEditor::create_entity(const std::string& name, const core::Guid& parent) {
    const core::Guid guid = core::Guid::generate();
    stack_.begin_transaction("Create " + name);
    const bool created =
        run("entity.create", {{"name", name}, {"parent", guid_json(parent)}, {"guid", guid.to_string()}});
    if (created) {
        set_selection({guid}, guid);
    }
    stack_.end_transaction();
    return created ? guid : core::Guid{};
}

void SceneEditor::delete_selection() {
    const std::vector<core::Guid> targets = selected_roots();
    if (targets.empty()) {
        return;
    }
    stack_.begin_transaction(targets.size() == 1 ? "Delete " + world_.name(world_.find(targets[0]))
                                                 : "Delete " + std::to_string(targets.size()) + " entities");
    set_selection({}, {});
    for (const core::Guid& guid : targets) {
        run("entity.delete", {{"entity", guid.to_string()}});
    }
    stack_.end_transaction();
}

void SceneEditor::duplicate_selection() {
    const std::vector<core::Guid> targets = selected_roots();
    if (targets.empty()) {
        return;
    }
    stack_.begin_transaction("Duplicate");
    std::vector<core::Guid> copies;
    for (const core::Guid& guid : targets) {
        const core::Guid copy = core::Guid::generate();
        if (run("entity.duplicate", {{"entity", guid.to_string()}, {"guid", copy.to_string()}})) {
            copies.push_back(copy);
        }
    }
    if (!copies.empty()) {
        set_selection(copies, copies.back());
    }
    stack_.end_transaction();
}

bool SceneEditor::rename(const core::Guid& entity, const std::string& name) {
    const entt::entity e = world_.find(entity);
    if (e == entt::null || world_.name(e) == name) {
        return false;
    }
    return run("entity.set_property", {{"entity", entity.to_string()}, {"path", "Name.value"}, {"value", name}});
}

bool SceneEditor::move_entities(const std::vector<core::Guid>& entities, const core::Guid& parent,
                                const core::Guid& before) {
    const entt::entity parent_e = parent.is_null() ? entt::null : world_.find(parent);
    if (!parent.is_null() && parent_e == entt::null) {
        return false;
    }
    // Keep the outermost moved entities, and never move an entity into its own subtree.
    std::vector<entt::entity> moving;
    for (const core::Guid& guid : entities) {
        const entt::entity e = world_.find(guid);
        if (e != entt::null && std::find(moving.begin(), moving.end(), e) == moving.end()) {
            moving.push_back(e);
        }
    }
    std::vector<entt::entity> roots;
    for (const entt::entity e : moving) {
        bool covered = false;
        for (entt::entity p = world_.parent(e); p != entt::null && !covered; p = world_.parent(p)) {
            covered = std::find(moving.begin(), moving.end(), p) != moving.end();
        }
        const bool cycle = parent_e != entt::null && world_.is_ancestor_or_self(e, parent_e);
        if (!covered && !cycle) {
            roots.push_back(e);
        }
    }
    // The anchor must not be one of the moved entities: use the next sibling that isn't.
    entt::entity anchor = before.is_null() ? entt::null : world_.find(before);
    if (anchor != entt::null) {
        if (world_.parent(anchor) != parent_e) {
            return false;
        }
        const std::vector<entt::entity> siblings = world_.children(parent_e);
        auto it = std::find(siblings.begin(), siblings.end(), anchor);
        while (it != siblings.end() && std::find(roots.begin(), roots.end(), *it) != roots.end()) {
            ++it;
        }
        anchor = it == siblings.end() ? entt::null : *it;
    }
    if (roots.empty()) {
        return false;
    }

    stack_.begin_transaction(roots.size() == 1 ? "Move " + world_.name(roots[0])
                                               : "Move " + std::to_string(roots.size()) + " entities");
    bool moved = false;
    for (const entt::entity e : roots) {
        // entity.reparent's index counts siblings after `e` left its old place.
        std::int64_t index = -1;
        if (anchor != entt::null) {
            index = static_cast<std::int64_t>(world_.sibling_index(anchor));
            if (world_.parent(e) == parent_e && world_.sibling_index(e) < world_.sibling_index(anchor)) {
                --index;
            }
        }
        moved |= run("entity.reparent",
                     {{"entity", world_.guid(e).to_string()}, {"parent", guid_json(parent)}, {"index", index}});
    }
    stack_.end_transaction();
    return moved;
}

core::Guid SceneEditor::create_layer(std::string name) {
    if (name.empty()) {
        for (int n = 1;; ++n) {
            name = "Layer " + std::to_string(n);
            const auto& layers = world_.layers().layers();
            if (std::none_of(layers.begin(), layers.end(), [&](const scene::LayerInfo& l) { return l.name == name; })) {
                break;
            }
        }
    }
    const core::Guid id = core::Guid::generate();
    return run("layer.create", {{"name", name}, {"guid", id.to_string()}}) ? id : core::Guid{};
}

bool SceneEditor::assign_selection(const core::Guid& layer) {
    const std::vector<core::Guid>& selected = world_.selection().entities();
    if (selected.empty()) {
        return false;
    }
    return run("layer.assign", {{"entities", guid_list(selected)}, {"layer", guid_json(layer)}});
}

std::vector<core::Guid> SceneEditor::layer_members(const core::Guid& layer) const {
    std::vector<core::Guid> out;
    for (const entt::entity e : world_.depth_first()) {
        if (scene::layer_of(world_, e) == layer) {
            out.push_back(world_.guid(e));
        }
    }
    return out;
}

std::string SceneEditor::save_bookmark(scene::CameraBookmark view) {
    if (view.name.empty()) {
        for (int n = 1;; ++n) {
            view.name = "View " + std::to_string(n);
            if (!world_.bookmarks().find(view.name)) {
                break;
            }
        }
    }
    return run("bookmark.save", core::reflect::to_json(view)) ? view.name : std::string();
}

bool SceneEditor::recall_bookmark(const std::string& name) {
    return run("bookmark.recall", {{"name", name}});
}

} // namespace f3d::editor
