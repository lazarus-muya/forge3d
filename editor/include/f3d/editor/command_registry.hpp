// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/editor/input.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::editor {

/// An editor command (UI action) with an optional default shortcut. Scene
/// changes are operations (M2), not commands; commands are things like
/// "reset layout" or "open the command palette".
struct Command {
    std::string id;   ///< `domain.verb`, e.g. "editor.reset_layout"
    std::string name; ///< shown in menus and the command palette
    std::function<void()> callback;
    std::optional<KeyChord> default_chord;
};

/// Commands plus the keymap: each command's chord is the user override if
/// there is one (possibly "unbound"), else its default.
class CommandRegistry {
public:
    /// Adds or replaces a command. Returns false (and keeps the old one) if `id` is empty.
    bool add(Command command);
    [[nodiscard]] const Command* find(std::string_view id) const;
    /// Runs a command by id; false if it doesn't exist.
    bool execute(std::string_view id) const;
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }

    // --- Keymap ---
    [[nodiscard]] std::optional<KeyChord> chord(std::string_view id) const;
    /// Overrides a command's chord; nullopt unbinds it.
    void set_chord(std::string_view id, std::optional<KeyChord> chord);
    /// Drops the override (back to the default chord).
    void reset_chord(std::string_view id);
    [[nodiscard]] bool is_overridden(std::string_view id) const;
    /// The command bound to `chord` (first one if several conflict).
    [[nodiscard]] const Command* find_by_chord(const KeyChord& chord) const;
    /// Groups of command ids that share a chord (each group has 2+ ids).
    [[nodiscard]] std::vector<std::vector<std::string>> conflicts() const;
    /// Ids that conflict with another command's chord.
    [[nodiscard]] bool has_conflict(std::string_view id) const;

    /// Overrides for Preferences::keymap ("" = unbound).
    [[nodiscard]] std::map<std::string, std::string> keymap_overrides() const;
    /// Applies Preferences::keymap; unknown ids are kept for commands registered later.
    void apply_keymap_overrides(const std::map<std::string, std::string>& overrides);

private:
    std::vector<Command> commands_;
    std::map<std::string, std::optional<KeyChord>, std::less<>> overrides_;
};

} // namespace f3d::editor
