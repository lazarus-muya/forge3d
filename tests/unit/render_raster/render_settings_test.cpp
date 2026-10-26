// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/camera.hpp"
#include "f3d/render/raster/capture.hpp"
#include "f3d/render/raster/render_settings.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <cmath>

using namespace f3d;
using namespace f3d::render::raster;

TEST_CASE("render settings: view modes and tonemappers have names") {
    for (const ViewMode mode :
         {ViewMode::Lit, ViewMode::Unlit, ViewMode::Wireframe, ViewMode::Normals, ViewMode::Cascades}) {
        CHECK(parse_view_mode(to_string(mode)) == mode);
    }
    for (const Tonemapper t : {Tonemapper::AgX, Tonemapper::Aces, Tonemapper::None}) {
        CHECK(parse_tonemapper(to_string(t)) == t);
    }
    CHECK_FALSE(parse_view_mode("sepia").has_value());
    CHECK_FALSE(parse_tonemapper("reinhard").has_value());
}

TEST_CASE("render settings: tonemappers map scene light into [0, 1] monotonically") {
    for (const Tonemapper t : {Tonemapper::AgX, Tonemapper::Aces, Tonemapper::None}) {
        float previous = -1.0f;
        for (float x = 0.0f; x < 64.0f; x = x * 1.5f + 0.001f) {
            const glm::vec3 y = tonemap(t, glm::vec3(x));
            CHECK(y.g >= previous - 1e-5f);
            CHECK(y.g >= 0.0f);
            CHECK(y.g <= 1.0f);
            previous = y.g;
        }
    }
    // Black stays black; bright light approaches white without clipping early.
    CHECK(tonemap(Tonemapper::AgX, glm::vec3(0.0f)).g < 0.01f);
    CHECK(tonemap(Tonemapper::Aces, glm::vec3(0.0f)).g < 0.01f);
    CHECK(tonemap(Tonemapper::AgX, glm::vec3(16.0f)).g > 0.9f);
    CHECK(tonemap(Tonemapper::Aces, glm::vec3(16.0f)).g > 0.95f);
    CHECK(tonemap(Tonemapper::None, glm::vec3(2.0f)) == glm::vec3(1.0f));
    // Mid grey (0.18) lands in the middle of the display range.
    for (const Tonemapper t : {Tonemapper::AgX, Tonemapper::Aces}) {
        const float grey = tonemap(t, glm::vec3(0.18f)).g;
        CHECK(grey > 0.1f);
        CHECK(grey < 0.45f);
    }
    // AgX desaturates bright saturated colours (the reason it's the default).
    const glm::vec3 red = tonemap(Tonemapper::AgX, glm::vec3(8.0f, 0.0f, 0.0f));
    CHECK(red.g > 0.2f);
    CHECK(red.r > red.g);
}

TEST_CASE("render settings: reflected-light metering") {
    // EV100 = log2(L * 100 / 12.5): 1 cd/m2 reads EV 3, 12.5 cd/m2 reads log2(100).
    CHECK(ev100_from_luminance(1.0f) == doctest::Approx(3.0f));
    CHECK(ev100_from_luminance(12.5f) == doctest::Approx(std::log2(100.0f)));
    CHECK(ev100_from_luminance(4000.0f) == doctest::Approx(std::log2(4000.0f * 8.0f)));
}

TEST_CASE("capture: bookmark views match the editor camera") {
    scene::World world;
    scene::CameraBookmark bookmark;
    bookmark.name = "Shot";
    bookmark.target = {1.0f, 2.0f, -3.0f};
    bookmark.distance = 7.5f;
    bookmark.yaw = 0.8f;
    bookmark.pitch = -0.3f;
    world.bookmarks().set(bookmark);

    CaptureRequest request;
    request.width = 1600;
    request.height = 900;
    request.bookmark = "Shot";
    const auto view = capture_view(world, nullptr, request, nullptr);
    REQUIRE(view.has_value());

    editor::EditorCamera camera;
    camera.target = bookmark.target;
    camera.distance = bookmark.distance;
    camera.yaw = bookmark.yaw;
    camera.pitch = bookmark.pitch;
    CHECK(glm::length(view->camera_position - camera.position()) < 1e-4f);
    const glm::mat4 expected = camera.projection_matrix(1600.0f / 900.0f) * camera.view_matrix();
    const glm::mat4 actual = view->view_projection();
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            CHECK(actual[c][r] == doctest::Approx(expected[c][r]).epsilon(1e-4));
        }
    }
    CHECK(view->width == 1600);

    request.bookmark = "Missing";
    std::string error;
    CHECK_FALSE(capture_view(world, nullptr, request, nullptr, &error).has_value());
    CHECK(error.find("Missing") != std::string::npos);
}

TEST_CASE("capture: a Camera entity's view and projection") {
    scene::World world;
    const entt::entity e = world.create("Cam");
    scene::Camera camera;
    camera.fov_y = 0.6f;
    camera.near_plane = 0.5f;
    camera.far_plane = 800.0f;
    world.registry().emplace<scene::Camera>(e, camera);
    const glm::quat rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    world.set_local(e, glm::vec3(4.0f, 1.0f, 2.0f), rotation, glm::vec3(2.0f)); // scale is ignored

    CaptureRequest request;
    request.width = 800;
    request.height = 800;
    request.camera = world.guid(e);
    const auto view = capture_view(world, nullptr, request, nullptr);
    REQUIRE(view.has_value());
    CHECK(glm::length(view->camera_position - glm::vec3(4.0f, 1.0f, 2.0f)) < 1e-5f);
    // The camera looks down its local -Z.
    const glm::vec3 forward = -glm::vec3(glm::inverse(view->view)[2]);
    CHECK(glm::length(forward - rotation * glm::vec3(0.0f, 0.0f, -1.0f)) < 1e-5f);
    CHECK(view->projection[1][1] == doctest::Approx(1.0f / std::tan(0.3f)));
    CHECK(view->near_plane == 0.5f);

    request.camera = core::Guid::generate();
    CHECK_FALSE(capture_view(world, nullptr, request, nullptr).has_value());
}

TEST_CASE("capture: without camera or bookmark, the host's view at the capture's aspect") {
    scene::World world;
    render::View host;
    host.camera_position = {0.0f, 3.0f, 9.0f};
    host.view = glm::lookAtRH(host.camera_position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    host.projection = glm::perspectiveRH_ZO(glm::radians(50.0f), 1.0f, 0.05f, 5000.0f);
    CaptureRequest request;
    request.width = 2000;
    request.height = 1000;
    const auto view = capture_view(world, nullptr, request, &host);
    REQUIRE(view.has_value());
    CHECK(view->view == host.view);
    CHECK(view->projection[1][1] == doctest::Approx(host.projection[1][1]));
    CHECK(view->projection[0][0] == doctest::Approx(host.projection[1][1] / 2.0f)); // 2:1 aspect

    // Nothing at all: a default framing.
    const auto framed = capture_view(world, nullptr, request, nullptr);
    REQUIRE(framed.has_value());
    CHECK(glm::length(framed->camera_position) > 1.0f);
}
