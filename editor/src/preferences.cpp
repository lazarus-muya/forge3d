// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/preferences.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace f3d::editor {
namespace {

using nlohmann::json;

constexpr const char* kFormat = "forge3d.preferences";

// --- Migrations: each takes version N and returns version N + 1. -----------------------------

/// v1 (flat): {"version":1, "ui_scale":1.5, "dark_theme":false, "navigation":"maya"}
/// v2 (grouped): {"format":..., "version":2, "ui":{"scale","theme"}, "navigation":{"preset"}, "keymap":{}}
json migrate_v1_to_v2(const json& v1) {
    json v2;
    v2["format"] = kFormat;
    v2["version"] = 2;
    v2["ui"]["scale"] = v1.value("ui_scale", 0.0f);
    v2["ui"]["theme"] = v1.value("dark_theme", true) ? "dark" : "light";
    v2["navigation"]["preset"] = v1.value("navigation", std::string("blender"));
    v2["keymap"] = json::object();
    return v2;
}

std::optional<Theme> parse_theme(std::string_view s) {
    if (s == "dark") {
        return Theme::Dark;
    }
    if (s == "light") {
        return Theme::Light;
    }
    return std::nullopt;
}

using ColorField = Rgba ThemeColors::*;
constexpr std::pair<const char*, ColorField> kColorFields[] = {
    {"accent", &ThemeColors::accent},
    {"window_bg", &ThemeColors::window_bg},
    {"text", &ThemeColors::text},
    {"selected", &ThemeColors::selected},
    {"active", &ThemeColors::active},
    {"edge", &ThemeColors::edge},
    {"face_selected", &ThemeColors::face_selected},
    {"face_hovered", &ThemeColors::face_hovered},
};

json color_json(const Rgba& c) {
    return json::array({c.r, c.g, c.b, c.a});
}

/// [r, g, b] or [r, g, b, a] in 0..1; anything else keeps `fallback`.
Rgba parse_color(const json& j, const Rgba& fallback) {
    if (!j.is_array() || j.size() < 3 || j.size() > 4 ||
        !std::all_of(j.begin(), j.end(), [](const json& v) { return v.is_number(); })) {
        return fallback;
    }
    const auto channel = [&](std::size_t i, float otherwise) {
        return i < j.size() ? std::clamp(j[i].get<float>(), 0.0f, 1.0f) : otherwise;
    };
    return {channel(0, 0.0f), channel(1, 0.0f), channel(2, 0.0f), channel(3, 1.0f)};
}

ThemeColors g_current_colors;

} // namespace

ThemeColors default_theme_colors(Theme theme) noexcept {
    ThemeColors colors;
    if (theme == Theme::Light) {
        colors.window_bg = {0.94f, 0.94f, 0.94f, 1.0f};
        colors.text = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return colors;
}

const ThemeColors& current_theme_colors() noexcept {
    return g_current_colors;
}

void set_current_theme_colors(const ThemeColors& colors) noexcept {
    g_current_colors = colors;
}

std::uint32_t packed(const Rgba& color) noexcept {
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(color.r) | (byte(color.g) << 8) | (byte(color.b) << 16) | (byte(color.a) << 24);
}

std::string_view to_string(Theme theme) noexcept {
    return theme == Theme::Dark ? "dark" : "light";
}

std::string to_json(const Preferences& p) {
    json j;
    j["format"] = kFormat;
    j["version"] = Preferences::kVersion;
    j["ui"]["scale"] = p.ui_scale;
    j["ui"]["theme"] = to_string(p.theme);
    for (const auto& [name, field] : kColorFields) {
        j["ui"]["colors"][name] = color_json(p.colors.*field);
    }
    j["navigation"]["preset"] = p.navigation_preset;
    j["files"]["autosave_minutes"] = p.autosave_minutes;
    j["streaming"]["vram_budget_mb"] = p.vram_budget_mb;
    j["automation"]["rpc_enabled"] = p.rpc_enabled;
    j["automation"]["rpc_port"] = p.rpc_port;
    j["automation"]["python_enabled"] = p.python_enabled;
    j["rendering"]["screen_percentage"] = p.screen_percentage;
    j["rendering"]["fog_quality"] = p.fog_quality;
    j["rendering"]["shadow_resolution"] = p.shadow_resolution;
    j["rendering"]["post_process"] = p.post_process;
    j["keymap"] = json::object();
    for (const auto& [id, chord] : p.keymap) {
        j["keymap"][id] = chord;
    }
    return j.dump(2) + "\n";
}

PreferencesResult parse_preferences(std::string_view text) {
    PreferencesResult result;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        result.error = "not valid JSON";
        return result;
    }
    const int version = doc.value("version", 0);
    result.loaded_version = version;
    if (version < 1 || version > Preferences::kVersion) {
        result.error = "unsupported preferences version " + std::to_string(version);
        return result;
    }
    if (version == 1) {
        doc = migrate_v1_to_v2(doc);
    }
    // Future: if (version <= 2) doc = migrate_v2_to_v3(doc); ...
    if (doc.value("format", "") != kFormat) {
        result.error = "missing \"format\": \"forge3d.preferences\"";
        return result;
    }

    Preferences p;
    const json ui = doc.value("ui", json::object());
    p.ui_scale = ui.value("scale", 0.0f);
    if (p.ui_scale != 0.0f && (p.ui_scale < 0.5f || p.ui_scale > 4.0f)) {
        p.ui_scale = 0.0f; // out of range: back to automatic
    }
    p.theme = parse_theme(ui.value("theme", std::string("dark"))).value_or(Theme::Dark);
    p.colors = default_theme_colors(p.theme);
    const json colors = ui.value("colors", json::object());
    if (colors.is_object()) {
        for (const auto& [name, field] : kColorFields) {
            if (colors.contains(name)) {
                p.colors.*field = parse_color(colors[name], p.colors.*field);
            }
        }
    }
    p.navigation_preset = doc.value("navigation", json::object()).value("preset", std::string("blender"));
    p.autosave_minutes = std::clamp(doc.value("files", json::object()).value("autosave_minutes", 5), 0, 240);
    p.vram_budget_mb = std::clamp(doc.value("streaming", json::object()).value("vram_budget_mb", 2048), 64, 65536);
    const json automation = doc.value("automation", json::object());
    p.rpc_enabled = automation.value("rpc_enabled", false);
    p.rpc_port = std::clamp(automation.value("rpc_port", 7468), 1024, 65535);
    p.python_enabled = automation.value("python_enabled", false);
    const json rendering = doc.value("rendering", json::object());
    p.screen_percentage = std::clamp(rendering.value("screen_percentage", 100.0f), 25.0f, 200.0f);
    p.fog_quality = rendering.value("fog_quality", std::string("high"));
    p.shadow_resolution = std::clamp(rendering.value("shadow_resolution", 2048), 256, 8192);
    p.post_process = rendering.value("post_process", true);
    const json keymap = doc.value("keymap", json::object());
    if (keymap.is_object()) {
        for (const auto& [id, chord] : keymap.items()) {
            if (chord.is_string()) {
                p.keymap[id] = chord.get<std::string>();
            }
        }
    }
    result.preferences = std::move(p);
    return result;
}

PreferencesResult load_preferences(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {std::nullopt, "cannot read " + path.string(), 0};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_preferences(ss.str());
}

bool save_preferences(const std::filesystem::path& path, const Preferences& preferences) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << to_json(preferences);
        if (!out.flush()) {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace f3d::editor
