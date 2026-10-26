// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/command_registry.hpp"

#include <algorithm>

namespace f3d::editor {

bool CommandRegistry::add(Command command) {
    if (command.id.empty()) {
        return false;
    }
    const auto it =
        std::find_if(commands_.begin(), commands_.end(), [&](const Command& c) { return c.id == command.id; });
    if (it != commands_.end()) {
        *it = std::move(command);
    } else {
        commands_.push_back(std::move(command));
    }
    return true;
}

const Command* CommandRegistry::find(std::string_view id) const {
    const auto it = std::find_if(commands_.begin(), commands_.end(), [&](const Command& c) { return c.id == id; });
    return it == commands_.end() ? nullptr : &*it;
}

bool CommandRegistry::execute(std::string_view id) const {
    const Command* command = find(id);
    if (!command || !command->callback) {
        return false;
    }
    command->callback();
    return true;
}

std::optional<KeyChord> CommandRegistry::chord(std::string_view id) const {
    if (const auto it = overrides_.find(id); it != overrides_.end()) {
        return it->second;
    }
    const Command* command = find(id);
    return command ? command->default_chord : std::nullopt;
}

void CommandRegistry::set_chord(std::string_view id, std::optional<KeyChord> chord) {
    overrides_[std::string(id)] = std::move(chord);
}

void CommandRegistry::reset_chord(std::string_view id) {
    if (const auto it = overrides_.find(id); it != overrides_.end()) {
        overrides_.erase(it);
    }
}

bool CommandRegistry::is_overridden(std::string_view id) const {
    return overrides_.find(id) != overrides_.end();
}

const Command* CommandRegistry::find_by_chord(const KeyChord& chord) const {
    for (const Command& c : commands_) {
        if (this->chord(c.id) == chord) {
            return &c;
        }
    }
    return nullptr;
}

std::vector<std::vector<std::string>> CommandRegistry::conflicts() const {
    std::vector<std::pair<KeyChord, std::vector<std::string>>> by_chord;
    for (const Command& c : commands_) {
        const auto k = chord(c.id);
        if (!k) {
            continue;
        }
        auto it = std::find_if(by_chord.begin(), by_chord.end(), [&](const auto& e) { return e.first == *k; });
        if (it == by_chord.end()) {
            by_chord.push_back({*k, {c.id}});
        } else {
            it->second.push_back(c.id);
        }
    }
    std::vector<std::vector<std::string>> out;
    for (auto& [k, ids] : by_chord) {
        if (ids.size() > 1) {
            out.push_back(std::move(ids));
        }
    }
    return out;
}

bool CommandRegistry::has_conflict(std::string_view id) const {
    const auto k = chord(id);
    if (!k) {
        return false;
    }
    return std::any_of(commands_.begin(), commands_.end(),
                       [&](const Command& c) { return c.id != id && chord(c.id) == k; });
}

std::map<std::string, std::string> CommandRegistry::keymap_overrides() const {
    std::map<std::string, std::string> out;
    for (const auto& [id, k] : overrides_) {
        out[id] = k ? to_string(*k) : std::string();
    }
    return out;
}

void CommandRegistry::apply_keymap_overrides(const std::map<std::string, std::string>& overrides) {
    for (const auto& [id, text] : overrides) {
        if (text.empty()) {
            set_chord(id, std::nullopt);
        } else if (const auto k = parse_key_chord(text)) {
            set_chord(id, *k);
        }
    }
}

} // namespace f3d::editor
