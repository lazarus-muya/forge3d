// SPDX-License-Identifier: Apache-2.0
#pragma once

// Editor-level scene edits built from operations (phases.md P2.4/P2.5): the
// actions behind the outliner, the layers panel, the edit commands and camera
// bookmarks. UI-free, so the rules (what a multi-entity drag does, which entities
// a delete touches) are unit tested. Every change goes through the OpStack.

#include "f3d/core/guid.hpp"
#include "f3d/scene/layers.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::ops {
class OpStack;
class OpRegistry;
} // namespace f3d::ops

namespace f3d::editor {

class SceneEditor {
public:
    SceneEditor(scene::World& world, ops::OpStack& stack, const ops::OpRegistry& registry);

    [[nodiscard]] scene::World& world() noexcept { return world_; }
    [[nodiscard]] ops::OpStack& stack() noexcept { return stack_; }

    /// Builds and runs an operation by id. Logs and returns false on failure.
    bool run(std::string_view id, const nlohmann::json& params, bool merge = false);

    // --- Selection ------------------------------------------------------------------------

    /// selection.set, only if it changes anything (no empty undo steps).
    void set_selection(std::vector<core::Guid> entities, core::Guid active = {});
    void select_all();
    /// Selected entities that still exist, minus those with a selected ancestor
    /// (operating on a parent already covers its children).
    [[nodiscard]] std::vector<core::Guid> selected_roots() const;

    // --- Entities ---------------------------------------------------------------------------

    /// Creates an empty entity under `parent` (null: a root) and selects it.
    core::Guid create_entity(const std::string& name, const core::Guid& parent = {});
    /// Deletes the selected roots and clears the selection, as one undo step.
    void delete_selection();
    /// Duplicates the selected roots and selects the copies, as one undo step.
    void duplicate_selection();
    bool rename(const core::Guid& entity, const std::string& name);

    /// Moves `entities` (in order) under `parent` (null: root level), before the
    /// sibling `before` (null: at the end), keeping their world transforms. Skips
    /// entities with a moved ancestor and moves that would create a cycle. One undo
    /// step; returns false if nothing moved.
    bool move_entities(const std::vector<core::Guid>& entities, const core::Guid& parent, const core::Guid& before);

    // --- Layers -----------------------------------------------------------------------------

    /// Creates a layer named "Layer N" (first free N) or `name`; returns its id.
    core::Guid create_layer(std::string name = {});
    /// Puts the selected entities on `layer` (null: the default layer).
    bool assign_selection(const core::Guid& layer);
    /// Entities directly on `layer` (null: the default layer), in scene order.
    [[nodiscard]] std::vector<core::Guid> layer_members(const core::Guid& layer) const;

    // --- Camera bookmarks -------------------------------------------------------------------

    /// Saves `view` as a bookmark; an empty name picks "View N".
    std::string save_bookmark(scene::CameraBookmark view);
    bool recall_bookmark(const std::string& name);

private:
    scene::World& world_;
    ops::OpStack& stack_;
    const ops::OpRegistry& registry_;
};

} // namespace f3d::editor
