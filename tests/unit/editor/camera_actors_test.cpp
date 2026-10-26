// SPDX-License-Identifier: Apache-2.0
// Scene cameras in the editor (editor::CameraActors): the view through a camera,
// adding one at the view, picking, pinning and piloting.
#include <doctest/doctest.h>

#include "f3d/editor/camera_actors.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <vector>

using namespace f3d;

namespace {

struct Fixture {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    editor::SceneEditor editor{world, stack, registry};
    editor::CameraActors cameras{world, editor};
    editor::EditorCamera view;

    Fixture() {
        scene::register_types();
        ops::register_builtin_ops(registry);
        view.target = glm::vec3(1.0f, 2.0f, 3.0f);
        view.distance = 5.0f;
        view.yaw = glm::radians(30.0f);
        view.pitch = glm::radians(-20.0f);
    }
};

bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return glm::length(a - b) < eps;
}

} // namespace

TEST_CASE("camera actors: aligning the editor camera to a camera matrix keeps eye and direction") {
    editor::EditorCamera source;
    source.target = glm::vec3(4.0f, 1.0f, -2.0f);
    source.distance = 7.0f;
    source.yaw = glm::radians(-120.0f);
    source.pitch = glm::radians(35.0f);
    const glm::mat4 m = editor::editor_camera_matrix(source);

    editor::EditorCamera aligned;
    aligned.distance = 3.0f;
    editor::align_editor_camera(aligned, m);
    CHECK(near(aligned.position(), source.position()));
    CHECK(near(aligned.forward(), source.forward()));
    CHECK(aligned.distance == 3.0f);

    // Looking straight down: the yaw comes from the right axis.
    source.pitch = glm::radians(-90.0f);
    editor::align_editor_camera(aligned, editor::editor_camera_matrix(source));
    CHECK(near(aligned.right(), source.right()));
}

TEST_CASE("camera actors: add camera at view creates a selected camera in one undo step") {
    Fixture f;
    const core::Guid guid = f.cameras.create_at_view(f.view);
    REQUIRE_FALSE(guid.is_null());
    CHECK(f.cameras.is_camera(guid));
    CHECK(f.cameras.name(guid) == "Camera 1");
    CHECK(f.cameras.selected_camera() == guid);
    CHECK(f.cameras.preview_camera() == guid);
    CHECK(f.stack.undo_count() == 1);

    // The view through it is the editor's view.
    const render::View through = *f.cameras.view(guid, 160, 90);
    CHECK(near(through.camera_position, f.view.position()));
    const glm::mat4 expected = f.view.view_matrix();
    for (int c = 0; c < 4; ++c) {
        CHECK(near(glm::vec3(through.view[c]), glm::vec3(expected[c])));
    }

    CHECK(f.cameras.name(f.cameras.create_at_view(f.view)) == "Camera 2");
    f.stack.undo();
    f.stack.undo();
    CHECK(f.world.size() == 0);
}

TEST_CASE("camera actors: a pinned camera stays previewed after deselection") {
    Fixture f;
    const core::Guid guid = f.cameras.create_at_view(f.view);
    f.editor.set_selection({}, {});
    CHECK(f.cameras.preview_camera().is_null());
    f.cameras.toggle_pin(guid);
    CHECK(f.cameras.preview_camera() == guid);
    f.cameras.toggle_pin(guid);
    CHECK(f.cameras.preview_camera().is_null());
}

TEST_CASE("camera actors: a camera is picked on screen near its body") {
    Fixture f;
    editor::EditorCamera placed = f.view;
    placed.target = glm::vec3(0.0f, 0.0f, -10.0f);
    placed.distance = 1.0f;
    placed.yaw = 0.0f;
    placed.pitch = 0.0f;
    const core::Guid guid = f.cameras.create_at_view(placed);

    // Seen from the origin looking down -Z, the camera sits at the centre of the image.
    editor::EditorCamera eye;
    eye.target = glm::vec3(0.0f, 0.0f, -5.0f);
    eye.distance = 5.0f;
    eye.yaw = 0.0f;
    eye.pitch = 0.0f;
    render::View view;
    view.width = 200;
    view.height = 100;
    view.view = eye.view_matrix();
    view.projection = eye.projection_matrix(2.0f);
    CHECK(f.cameras.pick(view, 100.0f, 50.0f, 10.0f) == guid);
    CHECK(f.cameras.pick(view, 10.0f, 10.0f, 10.0f).is_null());
    CHECK(f.cameras.pick_region(view, 80.0f, 30.0f, 120.0f, 70.0f) == std::vector<core::Guid>{guid});
}

TEST_CASE("camera actors: piloting moves the camera with the view, undoably") {
    Fixture f;
    const core::Guid guid = f.cameras.create_at_view(f.view);
    scene::Camera& lens = f.world.registry().get<scene::Camera>(f.world.find(guid));
    lens.fov_y = 1.0f;

    editor::EditorCamera camera;
    const float own_fov = camera.fov_y;
    REQUIRE(f.cameras.pilot(guid, camera));
    CHECK(camera.fov_y == doctest::Approx(1.0f));
    CHECK(near(camera.position(), f.view.position()));
    CHECK(f.cameras.preview_camera() == guid); // still previewed: its frame follows the pilot live

    const std::size_t before = f.stack.undo_count();
    camera.move(glm::vec3(0.0f, 0.0f, -2.0f)); // two frames of flying forward
    f.cameras.update(camera);
    camera.move(glm::vec3(0.0f, 0.0f, -2.0f));
    f.cameras.update(camera);
    f.cameras.update(camera); // the view stopped: the move becomes one undo step
    CHECK(f.stack.undo_count() == before + 1);
    const glm::vec3 moved = camera.position();
    CHECK(near(glm::vec3(f.world.world_matrix(f.world.find(guid))[3]), moved));

    // Undo moves the camera back, and the view follows it.
    f.stack.undo();
    f.cameras.update(camera);
    CHECK(near(camera.position(), f.view.position(), 1e-3f));

    f.cameras.eject(camera);
    CHECK(f.cameras.piloted().is_null());
    CHECK(camera.fov_y == own_fov);
}

TEST_CASE("camera actors: snap to view moves a camera to the editor's eye") {
    Fixture f;
    const core::Guid guid = f.cameras.create_at_view(f.view);
    editor::EditorCamera other = f.view;
    other.target = glm::vec3(-6.0f, 0.5f, 2.0f);
    other.yaw = glm::radians(170.0f);
    CHECK(f.cameras.snap_to_view(guid, other));
    const glm::mat4 world = f.world.world_matrix(f.world.find(guid));
    CHECK(near(glm::vec3(world[3]), other.position()));
    CHECK(near(-glm::normalize(glm::vec3(world[2])), other.forward()));
}
