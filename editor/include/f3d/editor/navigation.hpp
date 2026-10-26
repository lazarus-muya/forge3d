// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/math.hpp"
#include "f3d/editor/camera.hpp"
#include "f3d/editor/input.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::editor {

// --- Input maps (navigation presets, editor/resources/input/*.json) ---

enum class InputKind : std::uint8_t { Drag, Wheel, Key };

/// An action bound to an input chord: a mouse drag with a button, the wheel, or a key, plus modifiers.
struct InputBinding {
    std::string action; ///< e.g. "nav.orbit", "view.front", "fly.forward"
    InputKind kind = InputKind::Key;
    MouseButton button = MouseButton::None;
    std::string key;
    Modifiers modifiers;
};

struct NavigationSettings {
    float orbit_speed = 0.005f;     ///< radians per pixel
    float look_speed = 0.003f;      ///< radians per pixel (fly mode)
    float zoom_drag_speed = 0.01f;  ///< exponent per pixel
    float wheel_zoom_factor = 1.2f; ///< distance divided by this per notch
    float fly_speed = 6.0f;         ///< metres per second
    float fly_boost = 4.0f;         ///< speed multiplier while fly.boost is held
};

struct InputMap {
    std::string name;
    NavigationSettings settings;
    std::vector<InputBinding> bindings;

    [[nodiscard]] const InputBinding* find(InputKind kind, MouseButton button, std::string_view key,
                                           const Modifiers& modifiers) const;
};

struct InputMapResult {
    std::optional<InputMap> map;
    std::string error;
};

/// Parses a `forge3d.input_map` JSON document (format in docs/architecture/frame.md).
[[nodiscard]] InputMapResult parse_input_map(std::string_view json);
[[nodiscard]] InputMapResult load_input_map(const std::filesystem::path& path);

/// Directory holding the built-in presets (blender.json, maya.json, unreal.json).
[[nodiscard]] std::filesystem::path input_preset_dir();

// --- Navigation -------------------------------------------------------------------------

/// Walk / fly navigation (Blender's walk and fly modes): the mouse looks around without a
/// button held, W/A/S/D or the arrows move, Q/E go down/up, Shift is faster and the wheel
/// changes the speed. Walk moves level and falls onto the ground (NavigationController::
/// ground_height); fly goes where the camera looks. Tab switches between the two. A left
/// click, Enter or Space keeps the new view; a right-click or Escape goes back.
enum class FlightMode : std::uint8_t { None, Walk, Fly };

/// Turns input events into camera motion according to an InputMap.
class NavigationController {
public:
    void set_input_map(InputMap map);
    [[nodiscard]] const InputMap& input_map() const noexcept { return map_; }
    /// Viewport size in logical pixels (pan speed follows the cursor exactly).
    void set_viewport_size(glm::vec2 size) noexcept { viewport_ = size; }

    /// Returns true if the event was used for navigation.
    bool handle(const InputEvent& event, EditorCamera& camera);
    /// Continuous movement (fly mode); `dt` in seconds.
    void update(float dt, EditorCamera& camera);

    /// Fly speed multiplier (Unreal's camera speed); the wheel changes it while flying.
    [[nodiscard]] float speed_scale() const noexcept { return speed_scale_; }
    void set_speed_scale(float scale) noexcept { speed_scale_ = std::clamp(scale, kMinSpeedScale, kMaxSpeedScale); }
    static constexpr float kMinSpeedScale = 0.03125f;
    static constexpr float kMaxSpeedScale = 32.0f;
    static constexpr float kSpeedStep = 1.25f; ///< speed multiplier per wheel notch while flying

    /// True while a drag action (orbit, pan, zoom, fly) is in progress.
    [[nodiscard]] bool dragging() const noexcept { return !drag_action_.empty(); }

    // --- Walk / fly mode --------------------------------------------------------------------

    /// Starts walk or fly mode (None ends it, keeping the view). While it runs, handle()
    /// takes every event.
    void begin_flight(FlightMode mode, const EditorCamera& camera);
    /// Ends walk or fly mode: `keep` the new view, or go back to where it started.
    void end_flight(bool keep, EditorCamera& camera);
    [[nodiscard]] FlightMode flight() const noexcept { return flight_; }
    /// Walk mode's ground: the height of the ground under a point, if any (terrain, meshes).
    std::function<std::optional<float>(const glm::vec3& point)> ground_height;
    static constexpr float kEyeHeight = 1.7f; ///< walk: metres above the ground
    static constexpr float kGravity = 9.81f;
    /// Called for bound actions the camera doesn't handle itself (e.g. "view.frame_selection").
    std::function<void(std::string_view action)> on_action;

private:
    bool apply_key_action(const std::string& action, EditorCamera& camera);
    bool handle_flight(const InputEvent& event, EditorCamera& camera);
    void update_flight(float dt, EditorCamera& camera);

    InputMap map_;
    glm::vec2 viewport_{800.0f, 600.0f};
    std::string drag_action_;
    MouseButton drag_button_ = MouseButton::None;
    glm::vec2 last_position_{0.0f};
    std::vector<std::string> held_fly_actions_;
    float speed_scale_ = 1.0f;
    FlightMode flight_ = FlightMode::None;
    EditorCamera flight_start_;            ///< the view to go back to
    std::vector<std::string> flight_keys_; ///< keys held in walk / fly mode
    float fall_speed_ = 0.0f;              ///< walk: metres per second downwards
};

// --- Replay (--replay <input.json>) --------------------------------------------------------

struct ReplayEvent {
    std::uint64_t frame = 0; ///< delivered before rendering this frame (0-based)
    InputEvent event;
};

struct Replay {
    std::string preset;                 ///< navigation preset to switch to (empty: keep current)
    glm::vec2 viewport{800.0f, 600.0f}; ///< logical viewport size the events were recorded at
    std::vector<ReplayEvent> events;    ///< sorted by frame
    /// Optional "sculpt" object (terrain.sculpt_stroke's brush parameters, as JSON text):
    /// the sculpt tool is enabled with that brush and left-button events drive it.
    std::string sculpt;
};

struct ReplayResult {
    std::optional<Replay> replay;
    std::string error;
};

[[nodiscard]] ReplayResult parse_replay(std::string_view json);
[[nodiscard]] ReplayResult load_replay(const std::filesystem::path& path);

} // namespace f3d::editor
