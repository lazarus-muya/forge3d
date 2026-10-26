// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/editor/command_registry.hpp"
#include "f3d/editor/preferences.hpp"
#include "panel.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace f3d::editor {

namespace panel_ids {
inline constexpr const char* kKeymap = "keymap";
inline constexpr const char* kPreferences = "preferences";
} // namespace panel_ids

/// Keymap editor: every command with its shortcut; click a shortcut to rebind
/// it (the App forwards the next key chord), conflicts are highlighted.
class KeymapPanel final : public Panel {
public:
    KeymapPanel(CommandRegistry& commands, std::function<void()> on_changed);

    /// True while waiting for a key chord to bind.
    [[nodiscard]] bool capturing() const noexcept { return !capturing_id_.empty(); }
    /// Binds `chord` to the command being edited (Escape cancels, Backspace unbinds).
    void capture(const KeyChord& chord);
    /// The filter and the command table (also shown in Preferences > Keymap).
    void draw_editor();

protected:
    void draw_contents() override;

private:
    CommandRegistry& commands_;
    std::function<void()> on_changed_;
    std::string capturing_id_;
    std::array<char, 128> filter_{};
};

/// Preferences, Blender style: a sidebar of sections (Interface, Themes, Viewport,
/// Navigation, Keymap, Save & Load, System, Automation) and the chosen one beside it.
/// Changes apply immediately; `on_changed(save)` also asks to write the file (false
/// while a colour is still being dragged).
class PreferencesPanel final : public Panel {
public:
    PreferencesPanel(Preferences& preferences, const std::vector<std::string>& navigation_presets,
                     KeymapPanel* keymap, std::function<void(bool save)> on_changed);

protected:
    void draw_contents() override;

private:
    bool draw_interface();
    bool draw_themes(bool& save);
    bool draw_viewport();
    bool draw_navigation();
    bool draw_save_load();
    bool draw_system();
    bool draw_automation();

    Preferences& preferences_;
    const std::vector<std::string>& presets_;
    KeymapPanel* keymap_;
    std::function<void(bool save)> on_changed_;
    int section_ = 0;
    bool unsaved_ = false; ///< applied during a drag, not yet written
};

/// Ctrl+Shift+P: fuzzy search over all commands.
class CommandPalette {
public:
    explicit CommandPalette(CommandRegistry& commands) : commands_(commands) {}

    void open() noexcept { open_requested_ = true; }
    /// Draws the palette if open; runs the chosen command after closing it.
    void draw();

private:
    CommandRegistry& commands_;
    bool open_requested_ = false;
    bool focus_input_ = false;
    std::array<char, 128> query_{};
    int selected_ = 0;
};

} // namespace f3d::editor
