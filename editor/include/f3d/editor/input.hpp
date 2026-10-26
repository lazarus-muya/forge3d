// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/math.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace f3d::editor {

// Platform-independent input (SDL events and --replay files map to these).

enum class MouseButton : std::uint8_t { None, Left, Middle, Right };

struct Modifiers {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    friend bool operator==(const Modifiers&, const Modifiers&) = default;
};

enum class InputEventType : std::uint8_t { MouseDown, MouseUp, MouseMove, Wheel, KeyDown, KeyUp };

struct InputEvent {
    InputEventType type = InputEventType::MouseMove;
    MouseButton button = MouseButton::None; ///< MouseDown / MouseUp
    glm::vec2 position{0.0f};               ///< logical pixels, relative to the viewport
    glm::vec2 motion{0.0f};                 ///< MouseMove: relative motion (walk/fly look, relative mouse mode)
    float wheel = 0.0f;                     ///< Wheel: notches, > 0 = away from the user
    std::string key;                        ///< KeyDown / KeyUp: lower-case SDL key name, e.g. "f", "keypad 1"
    Modifiers modifiers;
};

/// A key plus modifiers, e.g. Ctrl+Shift+P. Keys are lower-case SDL key names.
struct KeyChord {
    std::string key;
    Modifiers modifiers;
    friend bool operator==(const KeyChord&, const KeyChord&) = default;
};

/// "Ctrl+Shift+P" (modifiers in Ctrl, Shift, Alt order; key title-cased).
[[nodiscard]] std::string to_string(const KeyChord& chord);
/// Parses "ctrl+shift+p", "Alt+Keypad 1", "F5" (case-insensitive). nullopt if malformed.
[[nodiscard]] std::optional<KeyChord> parse_key_chord(std::string_view text);
/// True for keys that are only modifiers (left ctrl, right shift, ...).
[[nodiscard]] bool is_modifier_key(std::string_view key) noexcept;

} // namespace f3d::editor
