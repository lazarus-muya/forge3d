// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scene panels (phases.md P2.5): the outliner (hierarchy, multi-select,
// drag-to-reparent, rename, search) and the layers panel. Every change goes
// through editor::SceneEditor, i.e. through operations.

#include "f3d/core/guid.hpp"
#include "panel.hpp"

#include <entt/entity/entity.hpp>

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

namespace f3d::editor {

class SceneEditor;

namespace panel_ids {
inline constexpr const char* kLayers = "layers";
} // namespace panel_ids

class OutlinerPanel final : public Panel {
public:
    explicit OutlinerPanel(SceneEditor* editor);

    /// Starts renaming the active entity (command edit.rename, F2).
    void begin_rename_active();

protected:
    void draw_contents() override;

private:
    enum class DropZone { Before, Inside, After };

    void draw_node(entt::entity e);
    void handle_click(entt::entity e);
    void handle_drop(entt::entity target, DropZone zone);
    void draw_context_menu(entt::entity e);
    void update_filter();

    SceneEditor* editor_ = nullptr;
    std::array<char, 128> search_{};
    std::string applied_search_;
    std::unordered_set<entt::entity> filter_visible_; ///< with a search: matches and their ancestors
    std::vector<entt::entity> visible_order_;         ///< rows drawn last frame, for Shift range selection
    std::vector<entt::entity> drawing_order_;         ///< rows drawn this frame
    core::Guid renaming_;                             ///< entity being renamed inline
    std::array<char, 256> rename_buffer_{};
    bool focus_rename_ = false;
    std::vector<core::Guid> dragged_;
    core::Guid scroll_to_;
    bool delete_requested_ = false; ///< Delete chosen in a context menu, done once the tree is drawn
};

class LayersPanel final : public Panel {
public:
    explicit LayersPanel(SceneEditor* editor);

protected:
    void draw_contents() override;

private:
    SceneEditor* editor_ = nullptr;
    core::Guid renaming_;
    std::array<char, 128> rename_buffer_{};
    bool focus_rename_ = false;
};

} // namespace f3d::editor
