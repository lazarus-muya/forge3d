// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace f3d::editor {

enum class Theme { Dark, Light };

/// A 0..1 RGBA colour (sRGB, as the UI shows it).
struct Rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    friend bool operator==(const Rgba&, const Rgba&) = default;
};

/// Colours the user can change on top of the theme: the UI's accent, background
/// and text, and the viewport's selection and mesh-editing overlays.
struct ThemeColors {
    Rgba accent{0.26f, 0.59f, 0.98f, 1.0f}; ///< buttons, sliders, headers, highlights
    Rgba window_bg{0.06f, 0.06f, 0.06f, 0.94f};
    Rgba text{1.0f, 1.0f, 1.0f, 1.0f};
    Rgba selected{0.94f, 0.49f, 0.16f, 1.0f};      ///< selected objects
    Rgba active{1.0f, 0.75f, 0.24f, 1.0f};         ///< the active (last selected) object
    Rgba edge{0.08f, 0.08f, 0.09f, 0.63f};         ///< mesh wireframe while modeling
    Rgba face_selected{1.0f, 0.55f, 0.08f, 1.0f};  ///< picked faces while modeling
    Rgba face_hovered{1.0f, 0.9f, 0.35f, 1.0f};    ///< the face under the cursor

    friend bool operator==(const ThemeColors&, const ThemeColors&) = default;
};

/// The colours `theme` starts with (background and text differ between dark and light).
[[nodiscard]] ThemeColors default_theme_colors(Theme theme) noexcept;

/// The colours the editor draws with now (set from the preferences when they change).
[[nodiscard]] const ThemeColors& current_theme_colors() noexcept;
void set_current_theme_colors(const ThemeColors& colors) noexcept;
/// Packed RGBA8 (red in the lowest byte), as debug lines take it.
[[nodiscard]] std::uint32_t packed(const Rgba& color) noexcept;

/// User preferences, stored as versioned JSON (`forge3d.preferences`) in the
/// user config directory. Older versions are migrated on load.
struct Preferences {
    static constexpr int kVersion = 2;

    float ui_scale = 0.0f; ///< 0 = follow the display's content scale
    Theme theme = Theme::Dark;
    ThemeColors colors; ///< optional in v2 files
    std::string navigation_preset = "blender";
    /// Minutes between autosaves of a modified scene (0: off). Optional in v2 files.
    int autosave_minutes = 5;
    /// VRAM the asset streamer may use for meshes and textures, in MB. Optional in v2 files.
    int vram_budget_mb = 2048;
    /// The JSON-RPC server (127.0.0.1 only, token-protected). Off by default. Optional in v2 files.
    bool rpc_enabled = false;
    int rpc_port = 7468;
    /// Running Python scripts in the editor (a separate permission from RPC). Off by default.
    bool python_enabled = false;
    /// Viewport rendering (Render menu). Optional in v2 files.
    float screen_percentage = 100.0f;      ///< 25-200
    std::string fog_quality = "high";      ///< off, low, medium, high, epic
    int shadow_resolution = 2048;          ///< pixels per cascade
    bool post_process = true;
    /// Keymap overrides: command id -> chord ("Ctrl+Shift+P"), "" = explicitly unbound.
    std::map<std::string, std::string> keymap;

    friend bool operator==(const Preferences&, const Preferences&) = default;
};

struct PreferencesResult {
    std::optional<Preferences> preferences;
    std::string error;
    int loaded_version = 0; ///< version found in the file (before migration)
};

[[nodiscard]] std::string to_json(const Preferences& preferences);
/// Parses and migrates any supported version (1..kVersion).
[[nodiscard]] PreferencesResult parse_preferences(std::string_view json);
[[nodiscard]] PreferencesResult load_preferences(const std::filesystem::path& path);
/// Writes atomically (temp file + rename). Returns false on I/O failure.
bool save_preferences(const std::filesystem::path& path, const Preferences& preferences);

[[nodiscard]] std::string_view to_string(Theme theme) noexcept;

} // namespace f3d::editor
