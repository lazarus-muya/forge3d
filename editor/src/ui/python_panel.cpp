// SPDX-License-Identifier: Apache-2.0
#include "python_panel.hpp"

#include <imgui.h>

#include <cstring>

namespace f3d::editor {

PythonPanel::PythonPanel(PythonActions actions) : Panel(panel_ids::kPython, "Python"), actions_(std::move(actions)) {
    set_open(false); // opened from the Window menu
}

void PythonPanel::run_input() {
    const std::string code(input_.data());
    if (code.empty() || !actions_.run_code) {
        return;
    }
    history_.push_back({code, true, false});
    const PythonOutput out = actions_.run_code(code);
    if (!out.text.empty()) {
        history_.push_back({out.text, false, !out.ok});
    }
    input_.fill('\0');
    scroll_to_bottom_ = true;
}

void PythonPanel::draw_contents() {
    if (!actions_.built) {
        ImGui::TextWrapped("This build has no Python. Configure with FORGE3D_WITH_PYTHON=ON (the -full presets).");
        return;
    }
    if (!actions_.enabled || !actions_.enabled()) {
        ImGui::TextWrapped("Python scripts are off. Turn them on in Preferences > Automation > Python scripts: "
                           "a script can do anything your user account can.");
        return;
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 4.5f;
    if (ImGui::BeginChild("##history", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
        for (const Entry& entry : history_) {
            if (entry.input) {
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), ">>> %s", entry.text.c_str());
            } else if (entry.error) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", entry.text.c_str());
            } else {
                ImGui::TextUnformatted(entry.text.c_str());
            }
        }
        if (scroll_to_bottom_) {
            ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom_ = false;
        }
    }
    ImGui::EndChild();

    ImGui::InputTextMultiline("##code", input_.data(), input_.size(),
                              ImVec2(-ImGui::GetFontSize() * 5.0f, ImGui::GetFrameHeight() * 2.2f),
                              ImGuiInputTextFlags_AllowTabInput);
    const bool submit = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl;
    ImGui::SameLine();
    if (ImGui::Button("Run") || submit) {
        run_input();
    }
    ImGui::TextDisabled("Ctrl+Enter runs. `forge3d` is imported; try forge3d.scene_query().");

    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7.0f);
    ImGui::InputTextWithHint("##script", "script.py", script_.data(), script_.size());
    ImGui::SameLine();
    if (ImGui::Button("Run Script") && script_[0] != '\0' && actions_.run_file) {
        const std::string path(script_.data());
        history_.push_back({"run " + path, true, false});
        const PythonOutput out = actions_.run_file(path);
        history_.push_back({out.text.empty() ? (out.ok ? "done" : "failed") : out.text, false, !out.ok});
        scroll_to_bottom_ = true;
    }
}

} // namespace f3d::editor
