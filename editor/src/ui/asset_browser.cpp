// SPDX-License-Identifier: Apache-2.0
#include "asset_browser.hpp"

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/asset_index.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/editor/thumbnail_cache.hpp"
#include "f3d/render/rhi/imgui_renderer.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace f3d::editor {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string join_tags(const std::vector<std::string>& tags) {
    std::string out;
    for (const std::string& tag : tags) {
        out += (out.empty() ? "" : ", ") + tag;
    }
    return out;
}

std::vector<std::string> split_tags(std::string_view text) {
    std::vector<std::string> tags;
    std::string current;
    const auto flush = [&] {
        while (!current.empty() && current.back() == ' ') {
            current.pop_back();
        }
        if (!current.empty() && std::find(tags.begin(), tags.end(), current) == tags.end()) {
            tags.push_back(current);
        }
        current.clear();
    };
    for (const char c : text) {
        if (c == ',') {
            flush();
        } else if (!(c == ' ' && current.empty())) {
            current.push_back(c);
        }
    }
    flush();
    return tags;
}

} // namespace

AssetBrowserPanel::AssetBrowserPanel(const assets::AssetDatabase* database, assets::AssetIndex* const* index,
                                     ThumbnailCache* thumbnails, AssetBrowserActions actions)
    : Panel(panel_ids::kAssetBrowser, "Asset Browser"), database_(database), index_(index), thumbnails_(thumbnails),
      actions_(std::move(actions)) {}

void AssetBrowserPanel::refresh() {
    assets::AssetQuery query;
    query.text = search_.data();
    query.type = type_filter_ == 1 ? "model" : type_filter_ == 2 ? "texture" : type_filter_ == 3 ? "prefab" : "";
    query.tags.assign(tag_filter_.begin(), tag_filter_.end());
    std::string key = query.text + "|" + query.type;
    for (const std::string& tag : query.tags) {
        key += "|" + tag;
    }
    const std::uint64_t version = database_ ? database_->version() : 0;
    if (key == last_query_ && version == last_version_) {
        return;
    }
    last_query_ = key;
    last_version_ = version;
    const auto start = std::chrono::steady_clock::now();
    results_.clear();
    if (index_ && *index_) {
        results_ = (*index_)->search(query);
    } else if (database_) {
        // No index (automated runs, untitled scenes): filter the database directly.
        const std::string needle = lower(query.text);
        for (const assets::AssetRecord& r : database_->records()) {
            const bool text_ok = needle.empty() || lower(r.path).find(needle) != std::string::npos;
            const bool type_ok = query.type.empty() || r.type() == query.type;
            const bool tags_ok = std::all_of(query.tags.begin(), query.tags.end(), [&](const std::string& t) {
                return std::find(r.tags.begin(), r.tags.end(), t) != r.tags.end();
            });
            if (text_ok && type_ok && tags_ok) {
                results_.push_back({r.guid, r.path, r.name(), std::string(r.type()), r.stale()});
            }
        }
    }
    last_query_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void AssetBrowserPanel::draw_toolbar() {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##search", "Search assets", search_.data(), search_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::Combo("##type", &type_filter_, "All types\0Models\0Textures\0Prefabs\0");
    ImGui::SameLine();
    const std::string tags_label =
        tag_filter_.empty() ? std::string("Tags") : "Tags (" + std::to_string(tag_filter_.size()) + ")";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::BeginCombo("##tags", tags_label.c_str())) {
        std::vector<std::string> all;
        if (index_ && *index_) {
            all = (*index_)->tags();
        } else if (database_) {
            std::set<std::string> unique;
            for (const auto& r : database_->records()) {
                unique.insert(r.tags.begin(), r.tags.end());
            }
            all.assign(unique.begin(), unique.end());
        }
        if (all.empty()) {
            ImGui::TextDisabled("No tags yet (right-click an asset)");
        }
        for (const std::string& tag : all) {
            bool on = tag_filter_.contains(tag);
            if (ImGui::Checkbox(tag.c_str(), &on)) {
                on ? (void)tag_filter_.insert(tag) : (void)tag_filter_.erase(tag);
            }
        }
        if (!tag_filter_.empty() && ImGui::Button("Clear")) {
            tag_filter_.clear();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(grid_ ? "List" : "Grid")) {
        grid_ = !grid_;
    }
    if (grid_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::SliderFloat("##size", &thumbnail_size_, 48.0f, 160.0f, "%.0f px");
    }
    ImGui::SameLine();
    if (actions_.rescan && ImGui::Button("Rescan")) {
        actions_.rescan();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu assets (%.1f ms)", results_.size(), last_query_ms_);
}

void AssetBrowserPanel::item_interactions(const assets::AssetSummary& item) {
    if (ImGui::IsItemClicked()) {
        selected_ = item.guid;
    }
    const bool placeable = item.type == "model" || item.type == "prefab";
    if (placeable && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && actions_.place) {
        actions_.place(item.guid);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        const assets::AssetRecord* record = database_ ? database_->find(item.guid) : nullptr;
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(item.path.c_str());
        ImGui::TextDisabled("%s · %s", item.type.c_str(), item.stale ? "not imported" : "imported");
        if (record && !record->tags.empty()) {
            ImGui::TextDisabled("tags: %s", join_tags(record->tags).c_str());
        }
        ImGui::TextDisabled("%s", item.guid.to_string().c_str());
        ImGui::EndTooltip();
    }
    const bool material = item.type == "material";
    if (material && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        actions_.assign_material) {
        actions_.assign_material(item.guid);
    }
    if ((placeable || item.type == "texture" || material) && ImGui::BeginDragDropSource()) {
        const std::string guid = item.guid.to_string();
        ImGui::SetDragDropPayload(kAssetPayload, guid.c_str(), guid.size() + 1);
        ImGui::Text(placeable ? "Place %s" : "Apply %s to a mesh", item.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem()) {
        selected_ = item.guid;
        if (placeable && ImGui::MenuItem("Place at 3D Cursor", nullptr, false, actions_.place != nullptr)) {
            actions_.place(item.guid);
        }
        if (material &&
            ImGui::MenuItem("Assign to Selection", nullptr, false, actions_.assign_material != nullptr)) {
            actions_.assign_material(item.guid);
        }
        if (ImGui::MenuItem("Re-import", nullptr, false, actions_.reimport != nullptr)) {
            actions_.reimport(item.guid);
        }
        if (ImGui::MenuItem("Edit Tags...", nullptr, false, actions_.set_tags != nullptr)) {
            editing_tags_ = item.guid;
            const assets::AssetRecord* record = database_ ? database_->find(item.guid) : nullptr;
            std::snprintf(tag_buffer_.data(), tag_buffer_.size(), "%s", record ? join_tags(record->tags).c_str() : "");
        }
        if (index_ && *index_) {
            const auto deps = (*index_)->dependencies(item.guid);
            const auto users = (*index_)->dependents(item.guid);
            if (!deps.empty() || !users.empty()) {
                ImGui::Separator();
            }
            for (const core::Guid& dep : deps) {
                const assets::AssetRecord* r = database_ ? database_->find(dep) : nullptr;
                ImGui::TextDisabled("uses %s", r ? r->path.c_str() : dep.to_string().c_str());
            }
            for (const core::Guid& user : users) {
                const assets::AssetRecord* r = database_ ? database_->find(user) : nullptr;
                ImGui::TextDisabled("used by %s", r ? r->path.c_str() : user.to_string().c_str());
            }
        }
        ImGui::EndPopup();
    }
}

void AssetBrowserPanel::draw_grid() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float cell = thumbnail_size_ + style.ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / cell));
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const int rows = (static_cast<int>(results_.size()) + columns - 1) / columns;
    ImGuiListClipper clipper;
    clipper.Begin(rows, thumbnail_size_ + line + style.ItemSpacing.y * 2.0f);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int col = 0; col < columns; ++col) {
                const std::size_t i = static_cast<std::size_t>(row * columns + col);
                if (i >= results_.size()) {
                    break;
                }
                const assets::AssetSummary& item = results_[i];
                if (col > 0) {
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                ImGui::PushID(item.guid.to_string().c_str());
                const assets::AssetRecord* record = database_ ? database_->find(item.guid) : nullptr;
                const render::rhi::Texture* texture = record && thumbnails_ ? &thumbnails_->get(*record) : nullptr;
                const ImVec2 size(thumbnail_size_, thumbnail_size_);
                const bool selected = selected_ == item.guid;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_HeaderActive]);
                }
                if (texture && *texture) {
                    ImGui::ImageButton(
                        "##thumb", static_cast<ImTextureID>(render::rhi::ImGuiRenderer::texture_id(*texture)), size);
                } else {
                    ImGui::Button(item.stale ? "importing..." : item.type.c_str(), size);
                }
                if (selected) {
                    ImGui::PopStyleColor();
                }
                item_interactions(item);
                // The name, clipped to the cell.
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::PushClipRect(pos, ImVec2(pos.x + thumbnail_size_, pos.y + line), true);
                ImGui::TextUnformatted(item.name.c_str());
                ImGui::PopClipRect();
                ImGui::PopID();
                ImGui::EndGroup();
            }
        }
    }
}

void AssetBrowserPanel::draw_list() {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##assets", 3, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(results_.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const assets::AssetSummary& item = results_[static_cast<std::size_t>(i)];
            ImGui::PushID(item.guid.to_string().c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Selectable(item.name.c_str(), selected_ == item.guid, ImGuiSelectableFlags_SpanAllColumns);
            item_interactions(item);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s%s", item.type.c_str(), item.stale ? " (not imported)" : "");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", item.path.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void AssetBrowserPanel::draw_tag_editor() {
    if (!editing_tags_.is_null()) {
        ImGui::OpenPopup("Edit tags");
    }
    if (ImGui::BeginPopupModal("Edit tags", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Comma-separated, e.g. \"rock, cliff\"");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool enter =
            ImGui::InputText("##tags", tag_buffer_.data(), tag_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
            if (actions_.set_tags) {
                actions_.set_tags(editing_tags_, split_tags(tag_buffer_.data()));
            }
            editing_tags_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            editing_tags_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AssetBrowserPanel::draw_contents() {
    F3D_ZONE;
    if (!database_ || database_->size() == 0) {
        ImGui::TextDisabled("No project assets");
        ImGui::TextDisabled("Save the scene as a .forge file: models and textures in its folder appear here.");
        return;
    }
    refresh();
    draw_toolbar();
    ImGui::Separator();
    if (ImGui::BeginChild("##items")) {
        grid_ ? draw_grid() : draw_list();
    }
    ImGui::EndChild();
    draw_tag_editor();
}

} // namespace f3d::editor
