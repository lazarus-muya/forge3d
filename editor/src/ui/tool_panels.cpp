// SPDX-License-Identifier: Apache-2.0
#include "tool_panels.hpp"

#include "f3d/editor/fuzzy.hpp"

#include <imgui.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace f3d::editor {
namespace {

const ImVec4 kConflictColor{0.95f, 0.40f, 0.35f, 1.0f};

/// Commands matching `query` (fuzzy on name and id), best first.
std::vector<const Command*> matching_commands(const CommandRegistry& commands, std::string_view query,
                                              std::string_view exclude = {}) {
    std::vector<std::pair<const Command*, int>> scored;
    for (const Command& c : commands.commands()) {
        if (c.id == exclude) {
            continue;
        }
        const auto by_name = fuzzy_score(query, c.name);
        const auto by_id = fuzzy_score(query, c.id);
        if (by_name || by_id) {
            scored.emplace_back(&c, std::max(by_name.value_or(-1000), by_id.value_or(-1000)));
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first->name < b.first->name;
    });
    std::vector<const Command*> out;
    for (const auto& [c, s] : scored) {
        out.push_back(c);
    }
    return out;
}

} // namespace

// --- Keymap ------------------------------------------------------------------------------------

KeymapPanel::KeymapPanel(CommandRegistry& commands, std::function<void()> on_changed)
    : Panel(panel_ids::kKeymap, "Keymap"), commands_(commands), on_changed_(std::move(on_changed)) {
    set_open(false);
}

void KeymapPanel::capture(const KeyChord& chord) {
    if (capturing_id_.empty()) {
        return;
    }
    if (chord.key == "escape" && chord.modifiers == Modifiers{}) {
        capturing_id_.clear();
        return;
    }
    if (chord.key == "backspace" && chord.modifiers == Modifiers{}) {
        commands_.set_chord(capturing_id_, std::nullopt);
    } else {
        commands_.set_chord(capturing_id_, chord);
    }
    capturing_id_.clear();
    if (on_changed_) {
        on_changed_();
    }
}

void KeymapPanel::draw_contents() {
    draw_editor();
}

void KeymapPanel::draw_editor() {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputTextWithHint("##filter", "Filter commands", filter_.data(), filter_.size());
    const auto conflicts = commands_.conflicts();
    if (!conflicts.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kConflictColor, "%zu conflicting shortcut%s", conflicts.size(),
                           conflicts.size() == 1 ? "" : "s");
    }
    if (capturing()) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                           "Press a shortcut for '%s'  (Escape cancels, Backspace unbinds)", capturing_id_.c_str());
    }
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##keymap", 3, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
    ImGui::TableHeadersRow();

    for (const Command* c : matching_commands(commands_, filter_.data())) {
        ImGui::PushID(c->id.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(c->name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", c->id.c_str());

        ImGui::TableSetColumnIndex(1);
        const auto chord = commands_.chord(c->id);
        const std::string label = capturing_id_ == c->id ? "Press a key..." : (chord ? to_string(*chord) : "-");
        const bool conflict = commands_.has_conflict(c->id);
        if (conflict) {
            ImGui::PushStyleColor(ImGuiCol_Text, kConflictColor);
        }
        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
            capturing_id_ = c->id;
        }
        if (conflict) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                std::string others;
                for (const auto& group : conflicts) {
                    if (std::find(group.begin(), group.end(), c->id) != group.end()) {
                        for (const auto& id : group) {
                            if (id != c->id) {
                                others += (others.empty() ? "" : ", ") + id;
                            }
                        }
                    }
                }
                ImGui::SetTooltip("Also bound to: %s", others.c_str());
            }
        }

        ImGui::TableSetColumnIndex(2);
        if (ImGui::SmallButton("Unbind")) {
            commands_.set_chord(c->id, std::nullopt);
            on_changed_();
        }
        if (commands_.is_overridden(c->id)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                commands_.reset_chord(c->id);
                on_changed_();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// --- Preferences --------------------------------------------------------------------------------

namespace {

/// The sidebar's sections, in order (as in Blender's Preferences).
enum class Section : int { Interface, Themes, Viewport, Navigation, Keymap, SaveLoad, System, Automation };
constexpr const char* kSectionNames[] = {"Interface", "Themes",      "Viewport", "Navigation",
                                         "Keymap",    "Save & Load", "System",   "Automation"};

/// A colour row with a picker; true when it changed.
bool color_row(const char* label, Rgba& color) {
    float rgba[4] = {color.r, color.g, color.b, color.a};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool changed =
        ImGui::ColorEdit4(label, rgba,
                          ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf |
                              ImGuiColorEditFlags_PickerHueWheel);
    if (changed) {
        color = {rgba[0], rgba[1], rgba[2], rgba[3]};
    }
    return changed;
}

} // namespace

PreferencesPanel::PreferencesPanel(Preferences& preferences, const std::vector<std::string>& navigation_presets,
                                   KeymapPanel* keymap, std::function<void(bool save)> on_changed)
    : Panel(panel_ids::kPreferences, "Preferences"), preferences_(preferences), presets_(navigation_presets),
      keymap_(keymap), on_changed_(std::move(on_changed)) {
    set_open(false);
}

void PreferencesPanel::draw_contents() {
    if (ImGui::BeginChild("##sections", ImVec2(ImGui::GetFontSize() * 9.0f, 0.0f), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(std::size(kSectionNames)); ++i) {
            if (i == static_cast<int>(Section::Keymap) || i == static_cast<int>(Section::SaveLoad)) {
                ImGui::Spacing(); // groups: look, input, files and system
            }
            if (ImGui::Selectable(kSectionNames[i], section_ == i)) {
                section_ = i;
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##section", ImVec2(0.0f, 0.0f))) {
        bool changed = false;
        bool save = true;
        switch (static_cast<Section>(section_)) {
        case Section::Interface: changed = draw_interface(); break;
        case Section::Themes: changed = draw_themes(save); break;
        case Section::Viewport: changed = draw_viewport(); break;
        case Section::Navigation: changed = draw_navigation(); break;
        case Section::Keymap:
            if (keymap_) {
                keymap_->draw_editor();
            }
            break;
        case Section::SaveLoad: changed = draw_save_load(); break;
        case Section::System: changed = draw_system(); break;
        case Section::Automation: changed = draw_automation(); break;
        }
        // A drag applies every frame but writes the file once, when nothing is active.
        if (changed && on_changed_) {
            on_changed_(save);
            unsaved_ = !save;
        } else if (unsaved_ && !ImGui::IsAnyItemActive() && on_changed_) {
            on_changed_(true);
            unsaved_ = false;
        }
    }
    ImGui::EndChild();
}

bool PreferencesPanel::draw_interface() {
    bool changed = false;
    ImGui::SeparatorText("Display");
    bool automatic = preferences_.ui_scale == 0.0f;
    if (ImGui::Checkbox("Automatic UI scale", &automatic)) {
        preferences_.ui_scale = automatic ? 0.0f : 1.0f;
        changed = true;
    }
    if (!automatic) {
        float scale = preferences_.ui_scale;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        ImGui::SliderFloat("UI scale", &scale, 0.5f, 3.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) { // apply on release, not every drag step
            preferences_.ui_scale = scale;
            changed = true;
        } else if (ImGui::IsItemActive()) {
            preferences_.ui_scale = scale;
        }
    }
    ImGui::TextDisabled("Light or dark theme and colours: Themes");
    return changed;
}

bool PreferencesPanel::draw_themes(bool& save) {
    bool changed = false;
    ThemeColors& colors = preferences_.colors;
    ImGui::SeparatorText("Theme");
    int theme = preferences_.theme == Theme::Dark ? 0 : 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::Combo("Base theme", &theme, "Dark\0Light\0")) {
        preferences_.theme = theme == 0 ? Theme::Dark : Theme::Light;
        // The background and text follow the base theme; the accent and viewport colours stay.
        const ThemeColors base = default_theme_colors(preferences_.theme);
        colors.window_bg = base.window_bg;
        colors.text = base.text;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset colours")) {
        colors = default_theme_colors(preferences_.theme);
        changed = true;
    }
    ImGui::SetItemTooltip("Back to the base theme's colours");

    const auto row = [&](const char* label, Rgba& color, const char* tooltip = nullptr) {
        changed |= color_row(label, color);
        if (tooltip) {
            ImGui::SetItemTooltip("%s", tooltip);
        }
    };
    ImGui::SeparatorText("Interface");
    row("Accent", colors.accent, "Buttons, sliders, tabs, selected rows and other highlights");
    row("Background", colors.window_bg);
    row("Text", colors.text);
    ImGui::SeparatorText("Viewport");
    row("Selected object", colors.selected);
    row("Active object", colors.active, "The last selected object, which tools and the inspector act on");
    row("Mesh edges", colors.edge, "The wireframe shown while modeling");
    row("Selected faces", colors.face_selected);
    row("Face under cursor", colors.face_hovered);
    save = !ImGui::IsAnyItemActive(); // still dragging or picking: save later
    return changed;
}

bool PreferencesPanel::draw_viewport() {
    bool changed = false;
    ImGui::SeparatorText("Quality");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    float percentage = preferences_.screen_percentage;
    ImGui::SliderFloat("Screen percentage", &percentage, 25.0f, 200.0f, "%.0f%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        preferences_.screen_percentage = percentage;
        changed = true;
    } else if (ImGui::IsItemActive()) {
        preferences_.screen_percentage = percentage;
    }
    ImGui::SetItemTooltip("Render resolution relative to the viewport (lower is faster)");
    static constexpr const char* kFog[] = {"off", "low", "medium", "high", "epic"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("Fog quality", preferences_.fog_quality.c_str())) {
        for (const char* quality : kFog) {
            if (ImGui::Selectable(quality, preferences_.fog_quality == quality)) {
                preferences_.fog_quality = quality;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    static constexpr int kShadow[] = {512, 1024, 2048, 4096};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("Shadow resolution", std::to_string(preferences_.shadow_resolution).c_str())) {
        for (const int size : kShadow) {
            if (ImGui::Selectable(std::to_string(size).c_str(), preferences_.shadow_resolution == size)) {
                preferences_.shadow_resolution = size;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::Checkbox("Post-processing", &preferences_.post_process);
    ImGui::SetItemTooltip("Bloom, exposure and tone mapping");
    ImGui::TextDisabled("Also in the Render menu.");
    return changed;
}

bool PreferencesPanel::draw_navigation() {
    bool changed = false;
    ImGui::SeparatorText("Preset");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("Navigation", preferences_.navigation_preset.c_str())) {
        for (const auto& preset : presets_) {
            if (ImGui::Selectable(preset.c_str(), preset == preferences_.navigation_preset)) {
                preferences_.navigation_preset = preset;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Mouse bindings for orbiting, panning and zooming. Shortcuts: Keymap");
    return changed;
}

bool PreferencesPanel::draw_save_load() {
    bool changed = false;
    ImGui::SeparatorText("Autosave");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    int minutes = preferences_.autosave_minutes;
    if (ImGui::SliderInt("Autosave (minutes)", &minutes, 0, 60, minutes == 0 ? "off" : "%d") &&
        minutes != preferences_.autosave_minutes) {
        preferences_.autosave_minutes = minutes;
        changed = true;
    }
    return changed;
}

bool PreferencesPanel::draw_system() {
    bool changed = false;
    ImGui::SeparatorText("Streaming");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    int vram_mb = preferences_.vram_budget_mb;
    if (ImGui::DragInt("Asset VRAM budget (MB)", &vram_mb, 16.0f, 64, 65536, "%d", ImGuiSliderFlags_AlwaysClamp) &&
        vram_mb != preferences_.vram_budget_mb) {
        preferences_.vram_budget_mb = vram_mb;
        changed = true;
    }
    return changed;
}

bool PreferencesPanel::draw_automation() {
    bool changed = false;
    ImGui::SeparatorText("JSON-RPC");
    bool rpc = preferences_.rpc_enabled;
    if (ImGui::Checkbox("JSON-RPC server (127.0.0.1)", &rpc)) {
        preferences_.rpc_enabled = rpc;
        changed = true;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    int port = preferences_.rpc_port;
    if (ImGui::InputInt("Port", &port, 0) && port >= 1024 && port <= 65535 && port != preferences_.rpc_port) {
        preferences_.rpc_port = port;
        changed = true;
    }
    ImGui::TextDisabled("Takes effect on the next start. Clients need the session token\n"
                        "the editor writes to rpc.json in your config directory.");
    ImGui::SeparatorText("Python");
    bool python = preferences_.python_enabled;
    if (ImGui::Checkbox("Python scripts (Window > Python)", &python)) {
        preferences_.python_enabled = python;
        changed = true;
    }
    ImGui::TextDisabled("A script can do anything your user account can: only run scripts you trust.");
    return changed;
}

// --- Command palette ----------------------------------------------------------------------------

void CommandPalette::draw() {
    constexpr const char* kPopup = "##command_palette";
    if (open_requested_) {
        ImGui::OpenPopup(kPopup);
        open_requested_ = false;
        focus_input_ = true;
        query_.fill('\0');
        selected_ = 0;
    }
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const float width = std::min(ImGui::GetFontSize() * 40.0f, main->WorkSize.x * 0.9f);
    ImGui::SetNextWindowPos(
        ImVec2(main->WorkPos.x + main->WorkSize.x * 0.5f, main->WorkPos.y + ImGui::GetFontSize() * 3.0f),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    if (!ImGui::BeginPopup(kPopup)) {
        return;
    }
    if (focus_input_) {
        ImGui::SetKeyboardFocusHere();
        focus_input_ = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##query", "Type a command", query_.data(), query_.size())) {
        selected_ = 0;
    }
    const auto matches = matching_commands(commands_, query_.data(), "editor.command_palette");
    const int count = static_cast<int>(matches.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        selected_ = std::min(selected_ + 1, count - 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        selected_ = std::max(selected_ - 1, 0);
    }
    const Command* chosen = nullptr;
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && count > 0) {
        chosen = matches[static_cast<std::size_t>(std::clamp(selected_, 0, count - 1))];
    }
    const int shown = std::min(count, 12);
    for (int i = 0; i < shown; ++i) {
        const Command* c = matches[static_cast<std::size_t>(i)];
        ImGui::PushID(c->id.c_str());
        if (ImGui::Selectable(c->name.c_str(), i == selected_)) {
            chosen = c;
        }
        if (const auto chord = commands_.chord(c->id)) {
            const std::string text = to_string(*chord);
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(text.c_str()).x);
            ImGui::TextDisabled("%s", text.c_str());
        }
        ImGui::PopID();
    }
    if (count == 0) {
        ImGui::TextDisabled("No matching commands");
    }
    if (chosen) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (chosen) {
        commands_.execute(chosen->id);
    }
}

} // namespace f3d::editor
