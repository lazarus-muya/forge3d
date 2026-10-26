// SPDX-License-Identifier: Apache-2.0
#include "panel.hpp"

#include "f3d/core/profile.hpp"

#include <imgui.h>

#include <algorithm>

namespace f3d::editor {

void Panel::draw() {
    if (!open_) {
        return;
    }
    F3D_ZONE;
    const std::string name = window_name();
    if (zero_padding()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }
    const bool visible = ImGui::Begin(name.c_str(), &open_);
    if (zero_padding()) {
        ImGui::PopStyleVar();
    }
    if (visible) {
        draw_contents();
    }
    ImGui::End();
}

Panel* PanelManager::find(std::string_view id) const {
    const auto it = std::find_if(panels_.begin(), panels_.end(), [&](const auto& p) { return p->id() == id; });
    return it == panels_.end() ? nullptr : it->get();
}

void PanelManager::draw_all() {
    for (const auto& panel : panels_) {
        panel->draw();
    }
}

} // namespace f3d::editor
