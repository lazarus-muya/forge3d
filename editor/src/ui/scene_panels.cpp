// SPDX-License-Identifier: Apache-2.0
#include "scene_panels.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string_view>

namespace f3d::editor {
namespace {

constexpr const char* kEntityPayload = "F3D_ENTITIES";

bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

void copy_to(std::array<char, 256>& buffer, const std::string& text) {
    std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
}

} // namespace

// --- Outliner -------------------------------------------------------------------------------

OutlinerPanel::OutlinerPanel(SceneEditor* editor) : Panel("outliner", "Outliner"), editor_(editor) {}

void OutlinerPanel::begin_rename_active() {
    if (!editor_) {
        return;
    }
    const scene::World& world = editor_->world();
    const core::Guid active = world.selection().active();
    const entt::entity e = world.find(active);
    if (e == entt::null) {
        return;
    }
    set_open(true);
    renaming_ = active;
    copy_to(rename_buffer_, world.name(e));
    focus_rename_ = true;
    scroll_to_ = active;
}

void OutlinerPanel::update_filter() {
    applied_search_ = search_.data();
    filter_visible_.clear();
    if (applied_search_.empty()) {
        return;
    }
    const scene::World& world = editor_->world();
    for (const entt::entity e : world.depth_first()) {
        if (contains_ci(world.name(e), applied_search_)) {
            for (entt::entity n = e; n != entt::null; n = world.parent(n)) {
                filter_visible_.insert(n);
            }
        }
    }
}

void OutlinerPanel::handle_click(entt::entity e) {
    scene::World& world = editor_->world();
    const core::Guid guid = world.guid(e);
    const ImGuiIO& io = ImGui::GetIO();
    const scene::Selection& selection = world.selection();
    if (io.KeyCtrl) {
        std::vector<core::Guid> next = selection.entities();
        const auto it = std::find(next.begin(), next.end(), guid);
        if (it == next.end()) {
            next.push_back(guid);
            editor_->set_selection(std::move(next), guid);
        } else {
            next.erase(it);
            editor_->set_selection(std::move(next), selection.active() == guid ? core::Guid{} : selection.active());
        }
    } else if (io.KeyShift && !selection.active().is_null()) {
        // Range from the active entity to this one, in the order the rows are shown.
        const entt::entity anchor = world.find(selection.active());
        const auto a = std::find(visible_order_.begin(), visible_order_.end(), anchor);
        const auto b = std::find(visible_order_.begin(), visible_order_.end(), e);
        if (a == visible_order_.end() || b == visible_order_.end()) {
            editor_->set_selection({guid}, guid);
            return;
        }
        std::vector<core::Guid> range;
        for (auto it = std::min(a, b); it <= std::max(a, b); ++it) {
            range.push_back(world.guid(*it));
        }
        editor_->set_selection(std::move(range), selection.active());
    } else {
        editor_->set_selection({guid}, guid);
    }
}

void OutlinerPanel::handle_drop(entt::entity target, DropZone zone) {
    scene::World& world = editor_->world();
    const std::vector<core::Guid> dragged = std::move(dragged_);
    dragged_.clear();
    if (target == entt::null) {
        editor_->move_entities(dragged, {}, {}); // dropped on the empty area: to the root level
        return;
    }
    const core::Guid target_guid = world.guid(target);
    const entt::entity parent = world.parent(target);
    const core::Guid parent_guid = parent == entt::null ? core::Guid{} : world.guid(parent);
    switch (zone) {
    case DropZone::Inside:
        editor_->move_entities(dragged, target_guid, {});
        break;
    case DropZone::Before:
        editor_->move_entities(dragged, parent_guid, target_guid);
        break;
    case DropZone::After: {
        const std::vector<entt::entity> siblings = world.children(parent);
        auto it = std::find(siblings.begin(), siblings.end(), target);
        const core::Guid next =
            (it != siblings.end() && it + 1 != siblings.end()) ? world.guid(*(it + 1)) : core::Guid{};
        editor_->move_entities(dragged, parent_guid, next);
        break;
    }
    }
}

void OutlinerPanel::draw_context_menu(entt::entity e) {
    if (!ImGui::BeginPopupContextItem()) {
        return;
    }
    scene::World& world = editor_->world();
    const core::Guid guid = world.guid(e);
    if (!world.selection().contains(guid)) {
        editor_->set_selection({guid}, guid); // right-click acts on the clicked entity
    }
    if (ImGui::MenuItem("Rename", "F2")) {
        renaming_ = guid;
        copy_to(rename_buffer_, world.name(e));
        focus_rename_ = true;
    }
    if (ImGui::MenuItem("Create Child")) {
        editor_->create_entity("Entity", guid);
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
        editor_->duplicate_selection();
    }
    if (ImGui::MenuItem("Delete", "Delete")) {
        delete_requested_ = true; // after the tree: the rows still being drawn may be the deleted ones
    }
    if (world.parent(e) != entt::null && ImGui::MenuItem("Move to Root")) {
        editor_->move_entities(editor_->selected_roots(), {}, {});
    }
    if (ImGui::BeginMenu("Layer")) {
        if (ImGui::MenuItem("Default")) {
            editor_->assign_selection({});
        }
        for (const scene::LayerInfo& layer : world.layers().layers()) {
            ImGui::PushID(layer.id.to_string().c_str());
            if (ImGui::MenuItem(layer.name.c_str(), nullptr, scene::layer_of(world, e) == layer.id)) {
                editor_->assign_selection(layer.id);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Layer")) {
            const core::Guid layer = editor_->create_layer();
            if (!layer.is_null()) {
                editor_->assign_selection(layer);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void OutlinerPanel::draw_node(entt::entity e) {
    scene::World& world = editor_->world();
    if (!applied_search_.empty() && !filter_visible_.contains(e)) {
        return;
    }
    drawing_order_.push_back(e);
    const core::Guid guid = world.guid(e);
    const bool selected = world.selection().contains(guid);
    const bool has_children = world.child_count(e) > 0;
    const bool visible = scene::is_visible(world, e);
    const bool pickable = scene::is_pickable(world, e);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth |
                               ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_NavLeftJumpsToParent;
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (!has_children) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (world.parent(e) == entt::null) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen; // roots start expanded
    }
    if (!applied_search_.empty()) {
        ImGui::SetNextItemOpen(true); // show every match
    }
    if (scroll_to_ == guid) {
        // Open the path to it so it's on screen.
        ImGui::SetScrollHereY();
        scroll_to_ = {};
    }

    ImGui::PushID(guid.to_string().c_str());
    const bool renaming = renaming_ == guid;
    const std::string& name = world.name(e);
    if (!visible) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    const char* state = renaming ? "" : (!visible ? "  (hidden)" : (!pickable ? "  (locked)" : ""));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s%s", renaming ? "" : name.c_str(), state);
    if (!visible) {
        ImGui::PopStyleColor();
    }
    const ImVec2 row_min = ImGui::GetItemRectMin();
    const ImVec2 row_max = ImGui::GetItemRectMax();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        handle_click(e);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        renaming_ = guid;
        copy_to(rename_buffer_, name);
        focus_rename_ = true;
    }
    draw_context_menu(e);

    // Drag source: the selection if this row is selected, else just this entity.
    if (ImGui::BeginDragDropSource()) {
        if (dragged_.empty()) {
            dragged_ = selected ? editor_->selected_roots() : std::vector<core::Guid>{guid};
        }
        const int count = static_cast<int>(dragged_.size());
        ImGui::SetDragDropPayload(kEntityPayload, &count, sizeof(count));
        if (count == 1) {
            ImGui::TextUnformatted(name.c_str());
        } else {
            ImGui::Text("%d entities", count);
        }
        ImGui::EndDragDropSource();
    }
    // Drop target: top quarter = before, bottom quarter = after (leaves/closed), else inside.
    if (ImGui::BeginDragDropTarget()) {
        const float y = ImGui::GetMousePos().y;
        const float h = row_max.y - row_min.y;
        DropZone zone = DropZone::Inside;
        if (y < row_min.y + h * 0.25f) {
            zone = DropZone::Before;
        } else if (y > row_max.y - h * 0.25f && !(has_children && open)) {
            zone = DropZone::After;
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
        if (zone == DropZone::Before) {
            draw->AddLine(ImVec2(row_min.x, row_min.y), ImVec2(row_max.x, row_min.y), color, 2.0f);
        } else if (zone == DropZone::After) {
            draw->AddLine(ImVec2(row_min.x, row_max.y), ImVec2(row_max.x, row_max.y), color, 2.0f);
        }
        if (ImGui::AcceptDragDropPayload(kEntityPayload,
                                         zone == DropZone::Inside ? 0 : ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            handle_drop(e, zone);
        }
        ImGui::EndDragDropTarget();
    }

    if (renaming) {
        // Inline rename over the row's label.
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(row_min.x + ImGui::GetTreeNodeToLabelSpacing(), row_min.y));
        ImGui::SetNextItemWidth(std::max(80.0f, row_max.x - row_min.x - ImGui::GetTreeNodeToLabelSpacing()));
        if (focus_rename_) {
            ImGui::SetKeyboardFocusHere();
            focus_rename_ = false;
        }
        const bool commit = ImGui::InputText("##rename", rename_buffer_.data(), rename_buffer_.size(),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (commit || ImGui::IsItemDeactivatedAfterEdit()) {
            if (rename_buffer_[0] != '\0') {
                editor_->rename(guid, rename_buffer_.data());
            }
            renaming_ = {};
        } else if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            renaming_ = {};
        }
    }

    if (open && has_children) {
        for (const entt::entity child : world.children(e)) {
            draw_node(child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void OutlinerPanel::draw_contents() {
    if (!editor_) {
        ImGui::TextDisabled("No scene");
        return;
    }
    F3D_ZONE;
    scene::World& world = editor_->world();
    if (ImGui::Button("+")) {
        editor_->create_entity("Entity");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create an empty entity");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search", search_.data(), search_.size());
    // Recompute the filter every frame while searching: the scene may change under it.
    if (search_[0] != '\0' || !applied_search_.empty()) {
        update_filter();
    }
    ImGui::Separator();

    if (ImGui::BeginChild("##tree")) {
        drawing_order_.clear();
        if (world.size() == 0) {
            ImGui::TextDisabled("Empty scene");
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f));
        for (const entt::entity root : world.roots()) {
            draw_node(root);
        }
        ImGui::PopStyleVar();
        visible_order_ = drawing_order_;
        if (delete_requested_) {
            delete_requested_ = false;
            editor_->delete_selection();
            visible_order_.clear(); // its rows may have been deleted
        }

        // The space below the rows: drop to move to the root level, click to deselect.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##empty", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 24.0f)));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
            editor_->set_selection({}, {});
        }
        if (ImGui::BeginDragDropTarget()) {
            if (ImGui::AcceptDragDropPayload(kEntityPayload)) {
                handle_drop(entt::null, DropZone::Inside);
            }
            ImGui::EndDragDropTarget();
        }
        if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload()) {
            dragged_.clear(); // a drag that ended outside any target
        }
    }
    ImGui::EndChild();
}

// --- Layers -----------------------------------------------------------------------------------

LayersPanel::LayersPanel(SceneEditor* editor) : Panel(panel_ids::kLayers, "Layers"), editor_(editor) {}

void LayersPanel::draw_contents() {
    if (!editor_) {
        ImGui::TextDisabled("No scene");
        return;
    }
    scene::World& world = editor_->world();
    if (ImGui::Button("New Layer")) {
        editor_->create_layer();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(world.selection().empty());
    if (ImGui::Button("Move Selection to Default")) {
        editor_->assign_selection({});
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##layers", 4, flags)) {
        return;
    }
    ImGui::TableSetupColumn("Show");
    ImGui::TableSetupColumn("Lock");
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("#");
    ImGui::TableHeadersRow();

    // The implicit default layer: always visible and unlocked.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(2);
    ImGui::TextDisabled("Default");
    ImGui::TableSetColumnIndex(3);
    ImGui::TextDisabled("%zu", editor_->layer_members({}).size());

    // Copy: ops below change the table while we iterate.
    const std::vector<scene::LayerInfo> layers = world.layers().layers();
    for (const scene::LayerInfo& layer : layers) {
        const std::string id = layer.id.to_string();
        ImGui::PushID(id.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        bool visible = layer.visible;
        if (ImGui::Checkbox("##visible", &visible)) {
            editor_->run("layer.set_visible", {{"layer", id}, {"value", visible}});
        }
        ImGui::TableSetColumnIndex(1);
        bool locked = layer.locked;
        if (ImGui::Checkbox("##locked", &locked)) {
            editor_->run("layer.set_locked", {{"layer", id}, {"value", locked}});
        }
        ImGui::TableSetColumnIndex(2);
        if (renaming_ == layer.id) {
            ImGui::SetNextItemWidth(-1.0f);
            if (focus_rename_) {
                ImGui::SetKeyboardFocusHere();
                focus_rename_ = false;
            }
            const bool commit =
                ImGui::InputText("##rename", rename_buffer_.data(), rename_buffer_.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (commit || ImGui::IsItemDeactivatedAfterEdit()) {
                if (rename_buffer_[0] != '\0' && layer.name != rename_buffer_.data()) {
                    editor_->run("layer.rename", {{"layer", id}, {"name", std::string(rename_buffer_.data())}});
                }
                renaming_ = {};
            } else if (ImGui::IsItemDeactivated()) {
                renaming_ = {};
            }
        } else {
            ImGui::Selectable(layer.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renaming_ = layer.id;
                std::snprintf(rename_buffer_.data(), rename_buffer_.size(), "%s", layer.name.c_str());
                focus_rename_ = true;
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Assign Selection", nullptr, false, !world.selection().empty())) {
                    editor_->assign_selection(layer.id);
                }
                if (ImGui::MenuItem("Select Entities")) {
                    const std::vector<core::Guid> members = editor_->layer_members(layer.id);
                    editor_->set_selection(members, members.empty() ? core::Guid{} : members.front());
                }
                if (ImGui::MenuItem("Rename")) {
                    renaming_ = layer.id;
                    std::snprintf(rename_buffer_.data(), rename_buffer_.size(), "%s", layer.name.c_str());
                    focus_rename_ = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Layer")) {
                    editor_->run("layer.delete", {{"layer", id}});
                }
                ImGui::EndPopup();
            }
        }
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%zu", editor_->layer_members(layer.id).size());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace f3d::editor
