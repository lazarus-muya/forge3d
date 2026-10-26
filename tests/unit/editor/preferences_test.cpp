// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/command_registry.hpp"
#include "f3d/editor/preferences.hpp"

#include <filesystem>
#include <fstream>

using namespace f3d::editor;
namespace fs = std::filesystem;

TEST_CASE("preferences: round-trip through JSON") {
    Preferences p;
    p.ui_scale = 1.25f;
    p.theme = Theme::Light;
    p.navigation_preset = "unreal";
    p.vram_budget_mb = 512;
    p.rpc_enabled = true;
    p.rpc_port = 9123;
    p.python_enabled = true;
    p.keymap = {{"editor.quit", "Ctrl+Shift+Q"}, {"file.open", ""}};

    const std::string json = to_json(p);
    CHECK(json.find("\"format\": \"forge3d.preferences\"") != std::string::npos);
    CHECK(json.find("\"version\": 2") != std::string::npos);
    const auto parsed = parse_preferences(json);
    REQUIRE(parsed.preferences.has_value());
    CHECK(parsed.loaded_version == Preferences::kVersion);
    CHECK(*parsed.preferences == p);
}

TEST_CASE("preferences: theme colours round-trip, default per theme and are clamped") {
    Preferences p;
    p.colors.accent = {0.9f, 0.2f, 0.3f, 1.0f};
    p.colors.edge = {0.1f, 0.1f, 0.1f, 0.5f};
    const auto parsed = parse_preferences(to_json(p));
    REQUIRE(parsed.preferences.has_value());
    CHECK(parsed.preferences->colors == p.colors);

    // Missing colours: the theme's defaults (a light background for the light theme).
    const auto light = parse_preferences(R"({"format": "forge3d.preferences", "version": 2, "ui": {"theme": "light"}})");
    REQUIRE(light.preferences.has_value());
    CHECK(light.preferences->colors == default_theme_colors(Theme::Light));
    CHECK(default_theme_colors(Theme::Light).window_bg != default_theme_colors(Theme::Dark).window_bg);

    const auto odd = parse_preferences(R"({"format": "forge3d.preferences", "version": 2,
        "ui": {"colors": {"accent": [2, -1, 0.5], "text": "red", "selected": [1, 1]}}})");
    REQUIRE(odd.preferences.has_value());
    const ThemeColors defaults = default_theme_colors(Theme::Dark);
    CHECK(odd.preferences->colors.accent == Rgba{1.0f, 0.0f, 0.5f, 1.0f}); // clamped, alpha 1
    CHECK(odd.preferences->colors.text == defaults.text);                 // not a colour
    CHECK(odd.preferences->colors.selected == defaults.selected);         // too short

    CHECK(packed(Rgba{1.0f, 0.0f, 0.0f, 1.0f}) == 0xFF0000FFu);
}

TEST_CASE("preferences: the RPC server is off unless enabled") {
    const auto parsed = parse_preferences(R"({"format": "forge3d.preferences", "version": 2})");
    REQUIRE(parsed.preferences.has_value());
    CHECK_FALSE(parsed.preferences->rpc_enabled);
    CHECK(parsed.preferences->rpc_port == 7468);
    CHECK_FALSE(parsed.preferences->python_enabled); // a separate permission, also off
    const auto low = parse_preferences(
        R"({"format": "forge3d.preferences", "version": 2, "automation": {"rpc_enabled": true, "rpc_port": 80}})");
    REQUIRE(low.preferences.has_value());
    CHECK(low.preferences->rpc_enabled);
    CHECK(low.preferences->rpc_port == 1024); // no privileged ports
}

TEST_CASE("preferences: version 1 file migrates to the current version") {
    const auto loaded = load_preferences(fs::path(F3D_TEST_DATA_DIR) / "preferences" / "v1.json");
    INFO(loaded.error);
    REQUIRE(loaded.preferences.has_value());
    CHECK(loaded.loaded_version == 1);
    const Preferences& p = *loaded.preferences;
    CHECK(p.ui_scale == doctest::Approx(1.5f));
    CHECK(p.theme == Theme::Light);
    CHECK(p.navigation_preset == "maya");
    CHECK(p.keymap.empty());
    CHECK(p.vram_budget_mb == 2048); // absent: the default 2 GB
    // Saved again, it is a current-version file.
    CHECK(parse_preferences(to_json(p)).loaded_version == Preferences::kVersion);
}

TEST_CASE("preferences: invalid input") {
    CHECK_FALSE(parse_preferences("not json").preferences);
    CHECK_FALSE(parse_preferences(R"({"version": 99})").preferences);
    CHECK_FALSE(parse_preferences(R"({"version": 2})").preferences); // v2 needs "format"
    // Out-of-range scale falls back to automatic; unknown theme to dark.
    const auto p = parse_preferences(
        R"({"format":"forge3d.preferences","version":2,"ui":{"scale":40,"theme":"neon"},"navigation":{"preset":"x"}})");
    REQUIRE(p.preferences);
    CHECK(p.preferences->ui_scale == 0.0f);
    CHECK(p.preferences->theme == Theme::Dark);
}

TEST_CASE("preferences: atomic save and load") {
    const auto dir = fs::temp_directory_path() / "forge3d-tests" / "prefs";
    fs::remove_all(dir);
    const auto path = dir / "nested" / "preferences.json";
    Preferences p;
    p.navigation_preset = "maya";
    REQUIRE(save_preferences(path, p)); // creates directories
    CHECK_FALSE(fs::exists(path.string() + ".tmp"));
    const auto loaded = load_preferences(path);
    REQUIRE(loaded.preferences);
    CHECK(*loaded.preferences == p);
}

TEST_CASE("keymap: chords format and parse") {
    const auto c = parse_key_chord("ctrl+shift+p");
    REQUIRE(c);
    CHECK(c->key == "p");
    CHECK(c->modifiers == Modifiers{true, true, false});
    CHECK(to_string(*c) == "Ctrl+Shift+P");
    CHECK(to_string(*parse_key_chord("Alt+Keypad 1")) == "Alt+Keypad 1");
    CHECK(parse_key_chord("F5")->key == "f5");
    CHECK(parse_key_chord("Ctrl+,")->key == ",");
    CHECK_FALSE(parse_key_chord(""));
    CHECK_FALSE(parse_key_chord("Ctrl+"));
    CHECK_FALSE(parse_key_chord("Hyper+X"));
    CHECK_FALSE(parse_key_chord("Ctrl+Left Shift")); // modifier-only
}

TEST_CASE("keymap: defaults, overrides and conflicts") {
    CommandRegistry r;
    int quit = 0;
    r.add({"editor.quit", "Quit", [&] { ++quit; }, parse_key_chord("Ctrl+Q")});
    r.add({"file.open", "Open", [] {}, parse_key_chord("Ctrl+O")});
    r.add({"editor.reset_layout", "Reset Layout", [] {}, std::nullopt});

    CHECK(r.find_by_chord(*parse_key_chord("Ctrl+Q"))->id == "editor.quit");
    CHECK(r.conflicts().empty());

    // Rebind reset_layout onto Ctrl+O: conflict with file.open.
    r.set_chord("editor.reset_layout", parse_key_chord("Ctrl+O"));
    const auto conflicts = r.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0] == std::vector<std::string>{"file.open", "editor.reset_layout"});
    CHECK(r.has_conflict("file.open"));
    CHECK_FALSE(r.has_conflict("editor.quit"));

    // Unbinding and resetting.
    r.set_chord("editor.quit", std::nullopt);
    CHECK_FALSE(r.chord("editor.quit"));
    CHECK(r.find_by_chord(*parse_key_chord("Ctrl+Q")) == nullptr);
    const auto overrides = r.keymap_overrides();
    CHECK(overrides.at("editor.quit").empty());
    CHECK(overrides.at("editor.reset_layout") == "Ctrl+O");
    r.reset_chord("editor.quit");
    CHECK(to_string(*r.chord("editor.quit")) == "Ctrl+Q");

    // Overrides from preferences.
    CommandRegistry fresh;
    fresh.add({"editor.quit", "Quit", [] {}, parse_key_chord("Ctrl+Q")});
    fresh.apply_keymap_overrides({{"editor.quit", "Ctrl+Shift+W"}, {"unknown.command", "F1"}});
    CHECK(to_string(*fresh.chord("editor.quit")) == "Ctrl+Shift+W");
}
