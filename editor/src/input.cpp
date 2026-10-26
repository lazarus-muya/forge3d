// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/input.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace f3d::editor {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return std::string(s.substr(first, last - first + 1));
}

/// "keypad 1" -> "Keypad 1", "p" -> "P".
std::string title_case(std::string_view key) {
    std::string out(key);
    bool start = true;
    for (char& c : out) {
        if (start) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        start = c == ' ';
    }
    return out;
}

} // namespace

std::string to_string(const KeyChord& chord) {
    std::string out;
    if (chord.modifiers.ctrl) {
        out += "Ctrl+";
    }
    if (chord.modifiers.shift) {
        out += "Shift+";
    }
    if (chord.modifiers.alt) {
        out += "Alt+";
    }
    return out + title_case(chord.key);
}

std::optional<KeyChord> parse_key_chord(std::string_view text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == '+') {
            parts.push_back(trim(current));
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(trim(current));
    if (parts.empty() || parts.back().empty()) {
        return std::nullopt;
    }
    KeyChord chord;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string m = lower(parts[i]);
        if (m == "ctrl" || m == "control") {
            chord.modifiers.ctrl = true;
        } else if (m == "shift") {
            chord.modifiers.shift = true;
        } else if (m == "alt") {
            chord.modifiers.alt = true;
        } else {
            return std::nullopt;
        }
    }
    chord.key = lower(parts.back());
    if (is_modifier_key(chord.key)) {
        return std::nullopt;
    }
    return chord;
}

bool is_modifier_key(std::string_view key) noexcept {
    for (const std::string_view m : {"left ctrl", "right ctrl", "left shift", "right shift", "left alt", "right alt",
                                     "left gui", "right gui", "ctrl", "shift", "alt"}) {
        if (key == m) {
            return true;
        }
    }
    return false;
}

} // namespace f3d::editor
