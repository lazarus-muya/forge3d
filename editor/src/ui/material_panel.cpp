// SPDX-License-Identifier: Apache-2.0
#include "material_panel.hpp"

#include "asset_browser.hpp"
#include "f3d/assets/asset_database.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/render/rhi/imgui_renderer.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"
#include "reflect_widgets.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>

namespace f3d::editor {
namespace {

/// The preview sphere and the parameters of `material` (stored at `ref`); edits go
/// through material.set_property when `editable`, else the widgets are disabled.
void draw_material(SceneEditor* editor, render::raster::MaterialPreview* preview, const core::AssetRef& ref,
                   const assets::Material& material, bool editable) {
    ImGui::SeparatorText("Preview");
    if (preview) {
        const float size = std::min(ImGui::GetContentRegionAvail().x, 192.0f * ImGui::GetIO().FontGlobalScale);
        const auto pixels =
            static_cast<std::uint32_t>(std::max(size, 16.0f) * ImGui::GetIO().DisplayFramebufferScale.x);
        const auto& texture = preview->render(material, pixels);
        ImGui::Image(static_cast<ImTextureID>(render::rhi::ImGuiRenderer::texture_id(texture)), ImVec2(size, size));
    }
    ImGui::SeparatorText("Parameters");
    editable = editable && editor;
    if (!editable) {
        ImGui::BeginDisabled();
    }
    assets::Material copy = material;
    entt::meta_any any = entt::forward_as_meta(copy);
    draw_reflected_component(any, [&](const std::string& path, const nlohmann::json& value, bool continuing) {
        if (!editable) {
            return;
        }
        const std::string property = path.substr(path.find('.') + 1); // "Material.roughness" -> "roughness"
        editor->run("material.set_property", {{"path", ref.path}, {"property", property}, {"value", value}},
                    continuing);
    });
    if (!editable) {
        ImGui::EndDisabled();
    }
}

/// The project's material records, sorted by path.
std::vector<const assets::AssetRecord*> project_materials(const assets::AssetDatabase* assets) {
    std::vector<const assets::AssetRecord*> out;
    if (assets) {
        for (const assets::AssetRecord& record : assets->records()) {
            if (record.type() == "material") {
                out.push_back(&record);
            }
        }
    }
    return out;
}

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    return std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != text.end();
}

} // namespace

bool is_editable_material(const core::AssetRef& ref) {
    return ref.path.ends_with(assets::kMaterialExtension) && ref.path.starts_with("project://");
}

// --- Material editor --------------------------------------------------------------------------

MaterialEditorPanel::MaterialEditorPanel(scene::World* world, assets::MeshLibrary* meshes, SceneEditor* editor,
                                         render::raster::MaterialPreview* preview, MaterialEditorActions actions)
    : Panel(panel_ids::kMaterialEditor, "Material Editor"), world_(world), meshes_(meshes), editor_(editor),
      preview_(preview), actions_(std::move(actions)) {
    set_open(false); // opened from Window ▸ Material Editor or the command palette
}

void MaterialEditorPanel::draw_contents() {
    if (!world_ || !meshes_) {
        ImGui::TextDisabled("No scene");
        return;
    }
    const core::Guid active = world_->selection().active();
    const entt::entity e = active.is_null() ? entt::null : world_->find(active);
    const auto* renderer = e == entt::null ? nullptr : world_->registry().try_get<scene::MeshRenderer>(e);
    if (!renderer) {
        ImGui::TextDisabled("Select an entity with a mesh");
        return;
    }
    if (active != entity_) {
        entity_ = active;
        slot_ = 0;
    }
    const auto mesh = meshes_->get(renderer->mesh);
    const std::vector<assets::SubMesh> submeshes = mesh ? assets::submeshes_of(*mesh) : std::vector<assets::SubMesh>{};
    if (submeshes.empty()) {
        ImGui::TextDisabled("The mesh isn't loaded");
        return;
    }
    slot_ = std::min(slot_, submeshes.size() - 1);

    // Slots.
    ImGui::SeparatorText("Slots");
    for (std::size_t i = 0; i < submeshes.size(); ++i) {
        const core::AssetRef ref = assets::MeshLibrary::slot_material(*renderer, i, submeshes[i]);
        const auto m = ref.empty() ? nullptr : meshes_->material(ref);
        const bool overridden = i < renderer->materials.size() && !renderer->materials[i].empty();
        const std::string label = std::to_string(i) + ": " + (m ? m->name : std::string("(default)")) +
                                  (overridden ? "  [assigned]" : "") + "##slot" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), slot_ == i)) {
            slot_ = i;
        }
        if (ImGui::IsItemHovered() && !ref.path.empty()) {
            ImGui::SetTooltip("%s", ref.path.c_str());
        }
        // A material dragged from the library or the asset browser onto a slot.
        if (actions_.assign_slot && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                if (const auto guid = core::Guid::parse(static_cast<const char*>(payload->Data))) {
                    actions_.assign_slot(entity_, i, *guid);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::TextDisabled("Drop a material from the library on a slot to assign it.");

    const core::AssetRef ref = assets::MeshLibrary::slot_material(*renderer, slot_, submeshes[slot_]);
    const auto material = ref.empty() ? nullptr : meshes_->material(ref);
    const bool editable = is_editable_material(ref);
    if (!editable) {
        ImGui::TextWrapped("%s", ref.empty() ? "The built-in default material."
                                             : "This material belongs to the model and is read-only.");
        if (actions_.make_editable_copy && ImGui::Button("Make Editable Copy")) {
            actions_.make_editable_copy(entity_, slot_);
        }
    } else {
        ImGui::TextDisabled("%s", ref.path.c_str());
    }
    draw_material(editor_, preview_, ref, material ? *material : assets::Material{}, editable);
}

// --- Material library -------------------------------------------------------------------------

MaterialLibraryPanel::MaterialLibraryPanel(const assets::AssetDatabase* assets, assets::MeshLibrary* meshes,
                                           SceneEditor* editor, render::raster::MaterialPreview* preview,
                                           MaterialLibraryActions actions)
    : Panel(panel_ids::kMaterialLibrary, "Material Library"), assets_(assets), meshes_(meshes), editor_(editor),
      preview_(preview), actions_(std::move(actions)) {
    set_open(false); // Window ▸ Material Library or the command palette
}

void MaterialLibraryPanel::create(const core::AssetRef& copy_from) {
    if (!actions_.create) {
        return;
    }
    const std::string path = actions_.create(copy_from);
    if (!path.empty()) {
        select_path_ = path;
    }
}

void MaterialLibraryPanel::draw_contents() {
    const std::vector<const assets::AssetRecord*> materials = project_materials(assets_);
    if (!select_path_.empty()) {
        for (const assets::AssetRecord* record : materials) {
            if (record->path == select_path_) {
                selected_ = record->guid;
                select_path_.clear();
            }
        }
    }
    const auto find = [&](const core::Guid& guid) -> const assets::AssetRecord* {
        const auto it = std::find_if(materials.begin(), materials.end(),
                                     [&](const assets::AssetRecord* r) { return r->guid == guid; });
        return it == materials.end() ? nullptr : *it;
    };
    const assets::AssetRecord* selected = find(selected_);
    const auto ref_of = [](const assets::AssetRecord& record) { return core::AssetRef{record.guid, record.path}; };
    const auto display_name = [&](const assets::AssetRecord& record) {
        const auto m = meshes_ ? meshes_->material(ref_of(record)) : nullptr;
        return m && !m->name.empty() ? m->name : record.name();
    };

    if (ImGui::Button("New Material")) {
        create({});
    }
    ImGui::SetItemTooltip("A new default material in project://materials/ (save the scene first)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!selected);
    if (ImGui::Button("Duplicate") && selected) {
        create(ref_of(*selected));
    }
    ImGui::SameLine();
    if (ImGui::Button("Assign to Selection") && selected && actions_.assign_to_selection) {
        actions_.assign_to_selection(selected->guid);
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter materials", &filter_);

    // The list.
    const float list_height = std::clamp(ImGui::GetTextLineHeightWithSpacing() * 8.0f, 80.0f,
                                         ImGui::GetContentRegionAvail().y * 0.4f);
    std::optional<core::Guid> remove;
    if (ImGui::BeginChild("##materials", ImVec2(0.0f, list_height), ImGuiChildFlags_Borders)) {
        if (materials.empty()) {
            ImGui::TextDisabled(assets_ ? "No materials yet: New Material makes one." : "No project open.");
        }
        for (const assets::AssetRecord* record : materials) {
            const std::string name = display_name(*record);
            if (!filter_.empty() && !contains_ignoring_case(name, filter_) &&
                !contains_ignoring_case(record->path, filter_)) {
                continue;
            }
            ImGui::PushID(record->path.c_str());
            if (renaming_ == record->guid) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (!ImGui::IsAnyItemActive()) {
                    ImGui::SetKeyboardFocusHere();
                }
                const bool done = ImGui::InputText("##rename", &rename_buffer_, ImGuiInputTextFlags_EnterReturnsTrue);
                if (done && editor_ && !rename_buffer_.empty() && rename_buffer_ != name) {
                    editor_->run("material.set_property",
                                 {{"path", record->path}, {"property", "name"}, {"value", rename_buffer_}});
                }
                if (done || ImGui::IsItemDeactivated()) {
                    renaming_ = {};
                }
            } else {
                if (ImGui::Selectable(name.c_str(), record->guid == selected_,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected_ = record->guid;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && actions_.assign_to_selection) {
                        actions_.assign_to_selection(record->guid);
                    }
                }
                ImGui::SetItemTooltip("%s\nDouble-click: assign to the selection. Drag onto a mesh or a slot.",
                                      record->path.c_str());
                if (ImGui::BeginDragDropSource()) {
                    const std::string guid = record->guid.to_string();
                    ImGui::SetDragDropPayload(kAssetPayload, guid.c_str(), guid.size() + 1);
                    ImGui::Text("Apply %s to a mesh", name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem()) {
                    selected_ = record->guid;
                    if (ImGui::MenuItem("Assign to Selection", nullptr, false,
                                        actions_.assign_to_selection != nullptr)) {
                        actions_.assign_to_selection(record->guid);
                    }
                    if (ImGui::MenuItem("Duplicate")) {
                        create(ref_of(*record));
                    }
                    if (ImGui::MenuItem("Rename", nullptr, false, editor_ != nullptr)) {
                        renaming_ = record->guid;
                        rename_buffer_ = name;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete", nullptr, false, actions_.remove != nullptr)) {
                        remove = record->guid;
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (remove && actions_.remove) {
        actions_.remove(*remove);
        if (selected_ == *remove) {
            selected_ = {};
        }
        selected = nullptr;
    }

    if (!selected) {
        ImGui::TextDisabled("Select a material to edit it.");
        return;
    }
    const core::AssetRef ref = ref_of(*selected);
    const auto material = meshes_ ? meshes_->material(ref) : nullptr;
    ImGui::TextDisabled("%s", selected->path.c_str());
    draw_material(editor_, preview_, ref, material ? *material : assets::Material{}, material != nullptr);
}

} // namespace f3d::editor
