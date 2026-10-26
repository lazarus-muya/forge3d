// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/navigation.hpp"

#include "f3d/core/system.hpp"
#include "f3d_editor_config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace f3d::editor {
namespace {

using nlohmann::json;

std::optional<MouseButton> parse_button(std::string_view s) {
    if (s == "left") {
        return MouseButton::Left;
    }
    if (s == "middle") {
        return MouseButton::Middle;
    }
    if (s == "right") {
        return MouseButton::Right;
    }
    return std::nullopt;
}

std::optional<Modifiers> parse_modifiers(const json& j) {
    Modifiers m;
    if (j.is_null()) {
        return m;
    }
    if (!j.is_array()) {
        return std::nullopt;
    }
    for (const auto& item : j) {
        if (!item.is_string()) {
            return std::nullopt;
        }
        const auto s = item.get<std::string>();
        if (s == "ctrl") {
            m.ctrl = true;
        } else if (s == "shift") {
            m.shift = true;
        } else if (s == "alt") {
            m.alt = true;
        } else {
            return std::nullopt;
        }
    }
    return m;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_camera_drag(std::string_view action) {
    return action == "nav.orbit" || action == "nav.pan" || action == "nav.zoom" || action == "nav.fly";
}

} // namespace

const InputBinding* InputMap::find(InputKind kind, MouseButton button, std::string_view key,
                                   const Modifiers& modifiers) const {
    for (const InputBinding& b : bindings) {
        if (b.kind != kind || b.modifiers != modifiers) {
            continue;
        }
        if ((kind == InputKind::Drag && b.button == button) || (kind == InputKind::Key && b.key == key) ||
            kind == InputKind::Wheel) {
            return &b;
        }
    }
    return nullptr;
}

InputMapResult parse_input_map(std::string_view text) {
    InputMapResult result;
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        result.error = "not valid JSON";
        return result;
    }
    if (doc.value("format", "") != "forge3d.input_map") {
        result.error = "missing \"format\": \"forge3d.input_map\"";
        return result;
    }
    if (doc.value("version", 0) != 1) {
        result.error = "unsupported input map version (expected 1)";
        return result;
    }
    InputMap map;
    map.name = doc.value("name", "");
    if (const auto it = doc.find("settings"); it != doc.end() && it->is_object()) {
        NavigationSettings& s = map.settings;
        s.orbit_speed = it->value("orbit_speed", s.orbit_speed);
        s.look_speed = it->value("look_speed", s.look_speed);
        s.zoom_drag_speed = it->value("zoom_drag_speed", s.zoom_drag_speed);
        s.wheel_zoom_factor = it->value("wheel_zoom_factor", s.wheel_zoom_factor);
        s.fly_speed = it->value("fly_speed", s.fly_speed);
        s.fly_boost = it->value("fly_boost", s.fly_boost);
    }
    const auto bindings = doc.find("bindings");
    if (bindings == doc.end() || !bindings->is_array()) {
        result.error = "missing \"bindings\" array";
        return result;
    }
    for (std::size_t i = 0; i < bindings->size(); ++i) {
        const json& b = (*bindings)[i];
        const auto fail = [&](const std::string& why) {
            result.error = "binding " + std::to_string(i) + ": " + why;
            return result;
        };
        if (!b.is_object() || !b.contains("action") || !b["action"].is_string()) {
            return fail("needs an \"action\" string");
        }
        InputBinding binding;
        binding.action = b["action"].get<std::string>();
        const std::string input = b.value("input", "");
        if (input == "drag") {
            binding.kind = InputKind::Drag;
            const auto button = parse_button(b.value("button", ""));
            if (!button) {
                return fail("drag needs \"button\": left|middle|right");
            }
            binding.button = *button;
        } else if (input == "wheel") {
            binding.kind = InputKind::Wheel;
        } else if (input == "key") {
            binding.kind = InputKind::Key;
            binding.key = lower(b.value("key", ""));
            if (binding.key.empty()) {
                return fail("key needs \"key\"");
            }
        } else {
            return fail("\"input\" must be drag, wheel or key");
        }
        const auto modifiers = parse_modifiers(b.value("modifiers", json()));
        if (!modifiers) {
            return fail("\"modifiers\" must be an array of ctrl/shift/alt");
        }
        binding.modifiers = *modifiers;
        map.bindings.push_back(std::move(binding));
    }
    result.map = std::move(map);
    return result;
}

InputMapResult load_input_map(const std::filesystem::path& path) {
    const auto text = read_file(path);
    if (!text) {
        return {std::nullopt, "cannot read " + path.string()};
    }
    auto result = parse_input_map(*text);
    if (!result.error.empty()) {
        result.error = path.filename().string() + ": " + result.error;
    }
    return result;
}

std::filesystem::path input_preset_dir() {
    std::error_code ec;
    const auto beside_exe = core::executable_dir() / "resources" / "input";
    if (std::filesystem::is_directory(beside_exe, ec)) {
        return beside_exe;
    }
    return std::filesystem::path(F3D_EDITOR_RESOURCE_DIR) / "input";
}

// --- NavigationController -----------------------------------------------------------------

void NavigationController::set_input_map(InputMap map) {
    map_ = std::move(map);
    drag_action_.clear();
    drag_button_ = MouseButton::None;
    held_fly_actions_.clear();
}

bool NavigationController::apply_key_action(const std::string& action, EditorCamera& camera) {
    if (action == "view.front") {
        camera.set_view(ViewPreset::Front);
    } else if (action == "view.back") {
        camera.set_view(ViewPreset::Back);
    } else if (action == "view.right") {
        camera.set_view(ViewPreset::Right);
    } else if (action == "view.left") {
        camera.set_view(ViewPreset::Left);
    } else if (action == "view.top") {
        camera.set_view(ViewPreset::Top);
    } else if (action == "view.bottom") {
        camera.set_view(ViewPreset::Bottom);
    } else if (action == "view.toggle_ortho") {
        camera.projection =
            camera.projection == Projection::Perspective ? Projection::Orthographic : Projection::Perspective;
    } else if (on_action) {
        on_action(action);
    } else {
        return false;
    }
    return true;
}

bool NavigationController::handle(const InputEvent& event, EditorCamera& camera) {
    if (flight_ != FlightMode::None) {
        return handle_flight(event, camera);
    }
    const NavigationSettings& s = map_.settings;
    switch (event.type) {
    case InputEventType::MouseDown: {
        if (dragging()) {
            return true; // one drag at a time; extra buttons are swallowed
        }
        const InputBinding* b = map_.find(InputKind::Drag, event.button, {}, event.modifiers);
        if (!b || !is_camera_drag(b->action)) {
            return false;
        }
        drag_action_ = b->action;
        drag_button_ = event.button;
        last_position_ = event.position;
        return true;
    }
    case InputEventType::MouseUp:
        if (dragging() && event.button == drag_button_) {
            drag_action_.clear();
            drag_button_ = MouseButton::None;
            held_fly_actions_.clear();
            return true;
        }
        return false;
    case InputEventType::MouseMove: {
        if (!dragging()) {
            return false;
        }
        const glm::vec2 d = event.position - last_position_;
        last_position_ = event.position;
        if (drag_action_ == "nav.orbit") {
            camera.orbit(-d.x * s.orbit_speed, -d.y * s.orbit_speed);
        } else if (drag_action_ == "nav.pan") {
            // The point under the cursor follows it (at the target's depth).
            const float units = camera.units_per_pixel(viewport_.y);
            camera.pan(-d.x * units, d.y * units);
        } else if (drag_action_ == "nav.zoom") {
            camera.zoom(std::exp(d.y * s.zoom_drag_speed));
        } else if (drag_action_ == "nav.fly") {
            camera.look(-d.x * s.look_speed, -d.y * s.look_speed);
        }
        return true;
    }
    case InputEventType::Wheel: {
        if (drag_action_ == "nav.fly" && event.wheel != 0.0f) {
            // Unreal: the wheel changes the camera speed while flying.
            speed_scale_ = std::clamp(speed_scale_ * std::pow(kSpeedStep, event.wheel), kMinSpeedScale, kMaxSpeedScale);
            return true;
        }
        const InputBinding* b = map_.find(InputKind::Wheel, MouseButton::None, {}, event.modifiers);
        if (!b || b->action != "nav.zoom" || event.wheel == 0.0f) {
            return false;
        }
        camera.zoom(std::pow(s.wheel_zoom_factor, -event.wheel));
        return true;
    }
    case InputEventType::KeyDown: {
        if (drag_action_ == "nav.fly") {
            // Fly keys ignore modifiers (shift is often the boost key).
            for (const InputBinding& b : map_.bindings) {
                if (b.kind == InputKind::Key && b.key == event.key && b.action.starts_with("fly.")) {
                    if (std::find(held_fly_actions_.begin(), held_fly_actions_.end(), b.action) ==
                        held_fly_actions_.end()) {
                        held_fly_actions_.push_back(b.action);
                    }
                    return true;
                }
            }
        }
        const InputBinding* b = map_.find(InputKind::Key, MouseButton::None, event.key, event.modifiers);
        if (!b || b->action.starts_with("fly.")) {
            return false;
        }
        return apply_key_action(b->action, camera);
    }
    case InputEventType::KeyUp: {
        bool used = false;
        for (const InputBinding& b : map_.bindings) {
            if (b.kind == InputKind::Key && b.key == event.key && b.action.starts_with("fly.")) {
                std::erase(held_fly_actions_, b.action);
                used = true;
            }
        }
        return used;
    }
    }
    return false;
}

void NavigationController::begin_flight(FlightMode mode, const EditorCamera& camera) {
    if (mode == FlightMode::None) {
        flight_ = FlightMode::None;
        flight_keys_.clear();
        return;
    }
    if (flight_ == FlightMode::None) {
        flight_start_ = camera;
        drag_action_.clear(); // a drag in progress gives way
        drag_button_ = MouseButton::None;
        held_fly_actions_.clear();
    }
    flight_ = mode;
    fall_speed_ = 0.0f;
}

void NavigationController::end_flight(bool keep, EditorCamera& camera) {
    if (flight_ == FlightMode::None) {
        return;
    }
    if (!keep) {
        camera = flight_start_;
    }
    flight_ = FlightMode::None;
    flight_keys_.clear();
}

bool NavigationController::handle_flight(const InputEvent& event, EditorCamera& camera) {
    const NavigationSettings& s = map_.settings;
    switch (event.type) {
    case InputEventType::MouseMove:
        camera.look(-event.motion.x * s.look_speed, -event.motion.y * s.look_speed);
        break;
    case InputEventType::MouseDown:
        if (event.button == MouseButton::Left) {
            end_flight(true, camera);
        } else if (event.button == MouseButton::Right) {
            end_flight(false, camera);
        }
        break;
    case InputEventType::Wheel:
        speed_scale_ = std::clamp(speed_scale_ * std::pow(kSpeedStep, event.wheel), kMinSpeedScale, kMaxSpeedScale);
        break;
    case InputEventType::KeyDown:
        if (event.key == "escape") {
            end_flight(false, camera);
        } else if (event.key == "return" || event.key == "space" || event.key == "keypad enter") {
            end_flight(true, camera);
        } else if (event.key == "tab") {
            flight_ = flight_ == FlightMode::Walk ? FlightMode::Fly : FlightMode::Walk;
            fall_speed_ = 0.0f;
        } else if (std::find(flight_keys_.begin(), flight_keys_.end(), event.key) == flight_keys_.end()) {
            flight_keys_.push_back(event.key);
        }
        break;
    case InputEventType::KeyUp:
        std::erase(flight_keys_, event.key);
        break;
    case InputEventType::MouseUp:
        break;
    }
    return true; // walk / fly mode takes every event
}

void NavigationController::update_flight(float dt, EditorCamera& camera) {
    glm::vec3 dir(0.0f); // x right, y up, z forward
    float speed = map_.settings.fly_speed * speed_scale_;
    for (const std::string& key : flight_keys_) {
        if (key == "w" || key == "up") {
            dir.z += 1.0f;
        } else if (key == "s" || key == "down") {
            dir.z -= 1.0f;
        } else if (key == "d" || key == "right") {
            dir.x += 1.0f;
        } else if (key == "a" || key == "left") {
            dir.x -= 1.0f;
        } else if (key == "e" || key == "page up") {
            dir.y += 1.0f;
        } else if (key == "q" || key == "page down") {
            dir.y -= 1.0f;
        } else if (key == "left shift" || key == "right shift") {
            speed *= map_.settings.fly_boost;
        }
    }
    if (flight_ == FlightMode::Fly) {
        if (glm::dot(dir, dir) > 0.0f) {
            camera.move(glm::normalize(glm::vec3(dir.x, dir.y, -dir.z)) * speed * dt);
        }
        return;
    }
    // Walk: level movement; Q/E still lift and lower when there is no ground to stand on.
    glm::vec3 forward = camera.forward();
    forward.y = 0.0f;
    forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 level = forward * dir.z + right * dir.x;
    if (glm::dot(level, level) > 0.0f) {
        camera.target += glm::normalize(level) * speed * dt;
    }
    const glm::vec3 eye = camera.position();
    const std::optional<float> ground = ground_height ? ground_height(eye) : std::nullopt;
    if (!ground) {
        camera.target.y += dir.y * speed * dt;
        fall_speed_ = 0.0f;
        return;
    }
    // Stand on the ground: step up onto what is below the eyes, fall onto what is further down.
    const float stand = *ground + kEyeHeight;
    if (eye.y <= stand) {
        camera.target.y += stand - eye.y;
        fall_speed_ = 0.0f;
    } else {
        fall_speed_ += kGravity * dt;
        camera.target.y -= std::min(fall_speed_ * dt, eye.y - stand);
    }
}

void NavigationController::update(float dt, EditorCamera& camera) {
    if (flight_ != FlightMode::None) {
        update_flight(dt, camera);
        return;
    }
    if (drag_action_ != "nav.fly" || held_fly_actions_.empty()) {
        return;
    }
    glm::vec3 dir(0.0f); // camera space: x right, y up, -z forward
    float speed = map_.settings.fly_speed * speed_scale_;
    for (const auto& action : held_fly_actions_) {
        if (action == "fly.forward") {
            dir.z -= 1.0f;
        } else if (action == "fly.back") {
            dir.z += 1.0f;
        } else if (action == "fly.right") {
            dir.x += 1.0f;
        } else if (action == "fly.left") {
            dir.x -= 1.0f;
        } else if (action == "fly.up") {
            dir.y += 1.0f;
        } else if (action == "fly.down") {
            dir.y -= 1.0f;
        } else if (action == "fly.boost") {
            speed *= map_.settings.fly_boost;
        }
    }
    if (glm::dot(dir, dir) > 0.0f) {
        camera.move(glm::normalize(dir) * speed * dt);
    }
}

// --- Replay -----------------------------------------------------------------------------------

ReplayResult parse_replay(std::string_view text) {
    ReplayResult result;
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        result.error = "not valid JSON";
        return result;
    }
    if (doc.value("format", "") != "forge3d.input_replay" || doc.value("version", 0) != 1) {
        result.error = "expected \"format\": \"forge3d.input_replay\", \"version\": 1";
        return result;
    }
    Replay replay;
    replay.preset = doc.value("preset", "");
    if (const auto it = doc.find("sculpt"); it != doc.end()) {
        if (!it->is_object()) {
            result.error = "\"sculpt\" must be an object of brush settings";
            return result;
        }
        replay.sculpt = it->dump();
    }
    if (const auto it = doc.find("viewport"); it != doc.end() && it->is_array() && it->size() == 2) {
        replay.viewport = {(*it)[0].get<float>(), (*it)[1].get<float>()};
    }
    const auto events = doc.find("events");
    if (events == doc.end() || !events->is_array()) {
        result.error = "missing \"events\" array";
        return result;
    }
    for (std::size_t i = 0; i < events->size(); ++i) {
        const json& e = (*events)[i];
        const auto fail = [&](const std::string& why) {
            result.error = "event " + std::to_string(i) + ": " + why;
            return result;
        };
        if (!e.is_object()) {
            return fail("must be an object");
        }
        ReplayEvent re;
        re.frame = e.value("frame", 0ull);
        const std::string type = e.value("type", "");
        InputEvent& ev = re.event;
        if (type == "mouse_down") {
            ev.type = InputEventType::MouseDown;
        } else if (type == "mouse_up") {
            ev.type = InputEventType::MouseUp;
        } else if (type == "mouse_move") {
            ev.type = InputEventType::MouseMove;
        } else if (type == "wheel") {
            ev.type = InputEventType::Wheel;
        } else if (type == "key_down") {
            ev.type = InputEventType::KeyDown;
        } else if (type == "key_up") {
            ev.type = InputEventType::KeyUp;
        } else {
            return fail("unknown type '" + type + "'");
        }
        if (ev.type == InputEventType::MouseDown || ev.type == InputEventType::MouseUp) {
            const auto button = parse_button(e.value("button", ""));
            if (!button) {
                return fail("needs \"button\"");
            }
            ev.button = *button;
        }
        ev.position = {e.value("x", 0.0f), e.value("y", 0.0f)};
        ev.wheel = e.value("wheel", 0.0f);
        ev.key = lower(e.value("key", ""));
        const auto modifiers = parse_modifiers(e.value("modifiers", json()));
        if (!modifiers) {
            return fail("bad \"modifiers\"");
        }
        ev.modifiers = *modifiers;
        replay.events.push_back(std::move(re));
    }
    std::stable_sort(replay.events.begin(), replay.events.end(),
                     [](const ReplayEvent& a, const ReplayEvent& b) { return a.frame < b.frame; });
    result.replay = std::move(replay);
    return result;
}

ReplayResult load_replay(const std::filesystem::path& path) {
    const auto text = read_file(path);
    if (!text) {
        return {std::nullopt, "cannot read " + path.string()};
    }
    return parse_replay(*text);
}

} // namespace f3d::editor
