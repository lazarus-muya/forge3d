// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/camera.hpp"
#include "f3d/editor/navigation.hpp"
#include "navigation_expectations.hpp"

#include <filesystem>
#include <string>

using namespace f3d::editor;

namespace {

InputMap load_preset(const std::string& name) {
    auto result = load_input_map(std::filesystem::path(F3D_EDITOR_PRESET_DIR) / (name + ".json"));
    INFO(result.error);
    REQUIRE(result.map.has_value());
    return *result.map;
}

InputEvent key(const char* name, Modifiers mods = {}) {
    InputEvent e;
    e.type = InputEventType::KeyDown;
    e.key = name;
    e.modifiers = mods;
    return e;
}

} // namespace

TEST_CASE("camera: orientation conventions") {
    EditorCamera cam;
    cam.target = glm::vec3(0.0f);
    cam.distance = 10.0f;
    cam.set_view(ViewPreset::Front);
    CHECK(glm::length(cam.position() - glm::vec3(0, 0, 10)) < 1e-4f); // front: camera on +Z
    cam.set_view(ViewPreset::Right);
    CHECK(glm::length(cam.position() - glm::vec3(10, 0, 0)) < 1e-4f); // right: camera on +X
    cam.set_view(ViewPreset::Top);
    CHECK(glm::length(cam.position() - glm::vec3(0, 10, 0)) < 1e-4f);
    CHECK(glm::length(cam.up() - glm::vec3(0, 0, -1)) < 1e-4f); // top view: screen-up is -Z

    // The view matrix maps the target to the screen centre, in front of the camera.
    cam.yaw = 0.7f;
    cam.pitch = -0.4f;
    const glm::vec4 v = cam.view_matrix() * glm::vec4(cam.target, 1.0f);
    CHECK(std::abs(v.x) < 1e-4f);
    CHECK(std::abs(v.y) < 1e-4f);
    CHECK(v.z == doctest::Approx(-10.0f).epsilon(1e-4));
}

TEST_CASE("camera: primitives") {
    EditorCamera cam;
    const glm::vec3 eye = cam.position();
    cam.look(0.3f, 0.1f); // first-person look keeps the eye in place
    CHECK(glm::length(cam.position() - eye) < 1e-4f);

    cam.pitch = 0.0f;
    cam.orbit(0.0f, 10.0f); // clamped at straight down/up
    CHECK(cam.pitch == doctest::Approx(glm::half_pi<float>()));

    const float d = cam.distance;
    cam.zoom(0.5f);
    CHECK(cam.distance == doctest::Approx(d * 0.5f));
    cam.zoom(0.0f);
    CHECK(cam.distance == doctest::Approx(EditorCamera::kMinDistance));

    f3d::core::Bounds b;
    b.add(glm::vec3(-1, 0, -1));
    b.add(glm::vec3(3, 2, 1));
    cam.frame(b);
    CHECK(glm::length(cam.target - glm::vec3(1, 1, 0)) < 1e-5f);
    CHECK(cam.distance > glm::length(b.extent()) * 0.5f);

    cam.projection = Projection::Orthographic;
    const glm::mat4 p = cam.projection_matrix(1.5f);
    // Orthographic: w stays 1.
    CHECK((p * glm::vec4(1, 2, -3, 1)).w == doctest::Approx(1.0f));
}

TEST_CASE("input map: every built-in preset parses") {
    for (const char* name : {"blender", "maya", "unreal"}) {
        INFO(name);
        const InputMap map = load_preset(name);
        CHECK_FALSE(map.name.empty());
        CHECK(map.find(InputKind::Wheel, MouseButton::None, {}, {}) != nullptr);
    }
}

TEST_CASE("input map: parse errors are reported") {
    CHECK(parse_input_map("{").error == "not valid JSON");
    CHECK_FALSE(parse_input_map(R"({"format":"other","version":1,"bindings":[]})").map);
    const auto bad_button = parse_input_map(
        R"({"format":"forge3d.input_map","version":1,"bindings":[{"action":"nav.orbit","input":"drag","button":"fourth"}]})");
    CHECK(bad_button.error.find("binding 0") != std::string::npos);
    const auto bad_mod = parse_input_map(
        R"({"format":"forge3d.input_map","version":1,"bindings":[{"action":"x","input":"key","key":"a","modifiers":["super"]}]})");
    CHECK_FALSE(bad_mod.map);
}

TEST_CASE("navigation: each preset's replay reaches the same expected camera") {
    for (const char* name : {"blender", "maya", "unreal"}) {
        INFO("preset ", name);
        auto replay =
            load_replay(std::filesystem::path(F3D_TEST_DATA_DIR) / "input" / ("nav_" + std::string(name) + ".json"));
        INFO(replay.error);
        REQUIRE(replay.replay.has_value());
        CHECK(replay.replay->preset == name);

        NavigationController nav;
        nav.set_input_map(load_preset(name));
        nav.set_viewport_size(replay.replay->viewport);
        EditorCamera cam;
        for (const ReplayEvent& e : replay.replay->events) {
            nav.handle(e.event, cam);
        }
        CHECK_FALSE(nav.dragging());
        const EditorCamera expected = test::expected_replay_camera();
        CHECK(cam.yaw == doctest::Approx(expected.yaw).epsilon(1e-5));
        CHECK(cam.pitch == doctest::Approx(expected.pitch).epsilon(1e-5));
        CHECK(cam.distance == doctest::Approx(expected.distance).epsilon(1e-5));
        CHECK(glm::length(cam.target - expected.target) < 1e-4f);
        CHECK(glm::length(cam.position() - expected.position()) < 1e-4f);
    }
}

TEST_CASE("navigation: view keys and chords are preset specific") {
    EditorCamera cam;
    NavigationController blender;
    blender.set_input_map(load_preset("blender"));
    CHECK(blender.handle(key("keypad 3"), cam));
    CHECK(cam.yaw == doctest::Approx(glm::half_pi<float>()));
    CHECK(blender.handle(key("keypad 5"), cam));
    CHECK(cam.projection == Projection::Orthographic);
    CHECK_FALSE(blender.handle(key("h", {false, false, true}), cam)); // Unreal/Maya chord, not Blender

    NavigationController unreal;
    unreal.set_input_map(load_preset("unreal"));
    CHECK(unreal.handle(key("j", {false, false, true}), cam)); // Alt+J: top
    CHECK(cam.pitch == doctest::Approx(-glm::half_pi<float>()));

    std::string frame_action;
    unreal.on_action = [&](std::string_view a) { frame_action = std::string(a); };
    CHECK(unreal.handle(key("f"), cam));
    CHECK(frame_action == "view.frame_selection");
}

TEST_CASE("navigation: fly mode moves while the look button is held") {
    EditorCamera cam;
    cam.pitch = 0.0f;
    cam.yaw = 0.0f; // looking down -Z
    NavigationController nav;
    nav.set_input_map(load_preset("unreal"));
    const glm::vec3 start = cam.position();

    nav.update(1.0f, cam);
    InputEvent w = key("w");
    CHECK_FALSE(nav.handle(w, cam)); // W does nothing outside fly mode

    InputEvent down;
    down.type = InputEventType::MouseDown;
    down.button = MouseButton::Right;
    REQUIRE(nav.handle(down, cam));
    CHECK(nav.dragging());
    CHECK(nav.handle(w, cam));
    nav.update(0.5f, cam); // 0.5 s at 6 m/s
    CHECK(glm::length(cam.position() - (start + glm::vec3(0, 0, -3))) < 1e-4f);

    InputEvent up = down;
    up.type = InputEventType::MouseUp;
    CHECK(nav.handle(up, cam));
    nav.update(1.0f, cam); // released: no more movement
    CHECK(glm::length(cam.position() - (start + glm::vec3(0, 0, -3))) < 1e-4f);
}

TEST_CASE("navigation: the wheel changes the camera speed while flying (Unreal)") {
    EditorCamera cam;
    cam.pitch = 0.0f;
    cam.yaw = 0.0f; // looking down -Z
    NavigationController nav;
    nav.set_input_map(load_preset("unreal"));

    InputEvent down;
    down.type = InputEventType::MouseDown;
    down.button = MouseButton::Right;
    REQUIRE(nav.handle(down, cam));
    const float distance = cam.distance;
    InputEvent wheel;
    wheel.type = InputEventType::Wheel;
    wheel.wheel = 2.0f;
    CHECK(nav.handle(wheel, cam));
    CHECK(cam.distance == distance); // no zoom while flying
    CHECK(nav.speed_scale() == doctest::Approx(NavigationController::kSpeedStep * NavigationController::kSpeedStep));

    const glm::vec3 start = cam.position();
    CHECK(nav.handle(key("w"), cam));
    nav.update(0.5f, cam);
    CHECK(glm::length(cam.position() - (start + glm::vec3(0, 0, -3) * nav.speed_scale())) < 1e-4f);

    nav.set_speed_scale(1000.0f);
    CHECK(nav.speed_scale() == NavigationController::kMaxSpeedScale);
}

TEST_CASE("navigation: walk and fly modes work with any preset (WASD, mouse look, keep or cancel)") {
    NavigationController nav;
    nav.set_input_map(load_preset("blender")); // no fly bindings of its own
    EditorCamera cam;
    cam.target = glm::vec3(0.0f, 5.0f, 0.0f);
    cam.distance = 10.0f;
    cam.yaw = 0.0f;
    cam.pitch = glm::radians(-30.0f); // looking down at the target
    const glm::vec3 start = cam.position();
    const auto release = [](const char* name) {
        InputEvent e = key(name);
        e.type = InputEventType::KeyUp;
        return e;
    };

    SUBCASE("walk moves level; fly moves where the camera looks") {
        nav.begin_flight(FlightMode::Walk, cam);
        CHECK(nav.handle(key("w"), cam));
        nav.update(1.0f, cam);
        CHECK(cam.position().y == doctest::Approx(start.y));
        CHECK(cam.position().z < start.z - 1.0f); // forward is -Z at yaw 0
        CHECK(nav.handle(key("tab"), cam));
        CHECK(nav.flight() == FlightMode::Fly);
        const float y = cam.position().y;
        nav.update(1.0f, cam);
        CHECK(cam.position().y < y - 1.0f); // down the view
        CHECK(nav.handle(release("w"), cam));
        const glm::vec3 still = cam.position();
        nav.update(1.0f, cam);
        CHECK(glm::length(cam.position() - still) < 1e-5f);
    }
    SUBCASE("the mouse looks around; a click keeps the view, Escape goes back") {
        nav.begin_flight(FlightMode::Fly, cam);
        InputEvent move;
        move.type = InputEventType::MouseMove;
        move.motion = {100.0f, 0.0f};
        const float yaw = cam.yaw;
        CHECK(nav.handle(move, cam));
        CHECK(cam.yaw != doctest::Approx(yaw));
        CHECK(glm::length(cam.position() - start) < 1e-4f); // looking turns in place
        CHECK(nav.handle(key("d"), cam));
        nav.update(0.5f, cam);
        CHECK(nav.handle(key("escape"), cam));
        CHECK(nav.flight() == FlightMode::None);
        CHECK(glm::length(cam.position() - start) < 1e-4f);
        CHECK(cam.yaw == doctest::Approx(yaw));

        nav.begin_flight(FlightMode::Fly, cam);
        CHECK(nav.handle(key("d"), cam));
        nav.update(0.5f, cam);
        InputEvent click;
        click.type = InputEventType::MouseDown;
        click.button = MouseButton::Left;
        CHECK(nav.handle(click, cam));
        CHECK(nav.flight() == FlightMode::None);
        CHECK(glm::length(cam.position() - start) > 1.0f);
        CHECK_FALSE(nav.handle(key("d"), cam)); // back to the preset's bindings
    }
    SUBCASE("walking falls onto the ground and climbs what is under the eyes") {
        float ground = 0.0f;
        nav.ground_height = [&](const glm::vec3&) { return std::optional<float>(ground); };
        nav.begin_flight(FlightMode::Walk, cam);
        for (int i = 0; i < 30; ++i) { // half a second of falling from 10 m
            nav.update(1.0f / 60.0f, cam);
        }
        CHECK(cam.position().y > NavigationController::kEyeHeight);
        CHECK(cam.position().y < start.y);
        for (int i = 0; i < 600; ++i) {
            nav.update(1.0f / 60.0f, cam);
        }
        CHECK(cam.position().y == doctest::Approx(NavigationController::kEyeHeight));
        ground = 0.5f; // a step
        nav.update(1.0f / 60.0f, cam);
        CHECK(cam.position().y == doctest::Approx(0.5f + NavigationController::kEyeHeight));
    }
}
