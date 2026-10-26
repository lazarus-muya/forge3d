// SPDX-License-Identifier: Apache-2.0
// Checks the camera an `forge3d-editor --replay nav_<preset>.json --bench out.json`
// run ended with (ctest fixture integration.editor.replay.<preset>).
#include <doctest/doctest.h>

#include "navigation_expectations.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

TEST_CASE("editor replay: final camera matches the scripted orbit/pan/zoom") {
    const char* path = std::getenv("F3D_REPLAY_BENCH");
    if (!path) {
        MESSAGE("F3D_REPLAY_BENCH not set; run through ctest integration.editor.replay.<preset>.check");
        return;
    }
    std::ifstream in(path);
    REQUIRE(in.good());
    const auto doc = nlohmann::json::parse(in, nullptr, false);
    REQUIRE_FALSE(doc.is_discarded());
    REQUIRE(doc.contains("camera"));
    const auto& cam = doc["camera"];
    INFO("preset ", cam.value("navigation_preset", "?"));

    const auto expected = f3d::editor::test::expected_replay_camera();
    CHECK(cam["yaw"].get<float>() == doctest::Approx(expected.yaw).epsilon(1e-4));
    CHECK(cam["pitch"].get<float>() == doctest::Approx(expected.pitch).epsilon(1e-4));
    CHECK(cam["distance"].get<float>() == doctest::Approx(expected.distance).epsilon(1e-4));
    const glm::vec3 position = expected.position();
    for (int i = 0; i < 3; ++i) {
        CHECK(cam["position"][i].get<float>() == doctest::Approx(position[i]).epsilon(1e-4));
        CHECK(cam["target"][i].get<float>() == doctest::Approx(expected.target[i]).epsilon(1e-4));
    }
}
