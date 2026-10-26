// SPDX-License-Identifier: Apache-2.0
// editor::ModelingTool's interactive edits: E / I / Ctrl+R follow the cursor, a click
// confirms (one undo step), cancel restores the mesh, the last edit can be adjusted.
#include <doctest/doctest.h>

#include "f3d/editor/modeling_tool.hpp"
#include "f3d/modeling/modeling_ops.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

using namespace f3d;
using modeling::EditableMesh;

namespace {

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    editor::ModelingTool tool{world, stack, registry};
    core::Guid cube = core::Guid::generate();

    Session() {
        modeling::register_types();
        ops::register_builtin_ops(registry);
        modeling::register_modeling_ops(registry);
        REQUIRE(stack.execute(registry.create("entity.create", {{"name", "Cube"}, {"guid", cube.to_string()}})));
        REQUIRE(stack.execute(registry.create("modeling.set_shape", {{"entity", cube.to_string()}, {"shape", "cube"}})));
        tool.set_target(cube);
        // Pick the top face (the 1 m cube rests on y = 0): a click from above.
        tool.update({0.1f, 5.0f, 0.1f}, {0.0f, -1.0f, 0.0f}, true, 1.0f);
        tool.update({0.1f, 5.0f, 0.1f}, {0.0f, -1.0f, 0.0f}, false, 1.0f);
        REQUIRE(tool.faces().size() == 1);
    }
    const EditableMesh& mesh() { return world.registry().get<EditableMesh>(world.find(cube)); }
    /// A cursor ray passing the cube's vertical axis at height `y`.
    void cursor_at(float y, bool pressed = false) { tool.update({5.0f, y, 0.0f}, {-1.0f, 0.0f, 0.0f}, pressed, 1.0f); }
    float top() { return modeling::face_centre(mesh(), tool.faces().front()).y; }
};

} // namespace

TEST_CASE("modeling tool: E extrudes with the mouse, a click confirms as one undo step") {
    Session s;
    const std::size_t undo = s.stack.undo_count();
    REQUIRE(s.tool.key_pressed("e", false, false, false));
    CHECK(s.tool.busy());
    s.cursor_at(2.0f); // the reference: nothing moved yet
    CHECK(s.mesh().face_count() == 10);
    CHECK(s.top() == doctest::Approx(1.0f));
    s.cursor_at(2.5f); // half a metre up
    CHECK(s.tool.modal_value() == doctest::Approx(0.5f));
    CHECK(s.top() == doctest::Approx(1.5f)); // previewed in the mesh
    CHECK(s.stack.undo_count() == undo);      // nothing recorded yet
    CHECK(s.tool.key_pressed("z", true, false, false)); // shortcuts are swallowed meanwhile
    s.cursor_at(2.5f, true);
    CHECK_FALSE(s.tool.busy());
    CHECK(s.stack.undo_count() == undo + 1);
    CHECK(s.top() == doctest::Approx(1.5f));

    SUBCASE("the last edit can be adjusted in place") {
        REQUIRE(s.tool.last_edit());
        CHECK(s.tool.last_edit()->kind == editor::ModelingTool::Edit::Extrude);
        REQUIRE(s.tool.adjust_last(0.25f));
        CHECK(s.stack.undo_count() == undo + 1);
        CHECK(s.top() == doctest::Approx(1.25f));
        REQUIRE(s.stack.undo());
        CHECK(s.mesh().face_count() == 6);
        CHECK(s.tool.last_edit() == nullptr); // undone: nothing to adjust
    }
}

TEST_CASE("modeling tool: cancel restores the mesh; typed values are exact") {
    Session s;
    const EditableMesh before = s.mesh();
    const std::size_t undo = s.stack.undo_count();
    REQUIRE(s.tool.begin_inset());
    s.tool.update({0.6f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, false, 1.0f); // the reference
    s.tool.update({0.2f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, false, 1.0f); // towards the centre
    CHECK(s.tool.modal_value() == doctest::Approx(1.0f - 0.2f / 0.6f));
    CHECK_FALSE(s.mesh() == before);
    s.tool.cancel(); // right-click / Escape
    CHECK_FALSE(s.tool.busy());
    CHECK(s.mesh() == before);
    CHECK(s.stack.undo_count() == undo);
    CHECK(s.tool.faces().size() == 1); // cancelling the edit keeps the picked faces

    REQUIRE(s.tool.key_pressed("e", false, false, false));
    s.cursor_at(2.0f);
    for (const char* key : {"keypad 2", "period", "5"}) {
        REQUIRE(s.tool.key_pressed(key, false, false, false));
    }
    REQUIRE(s.tool.key_pressed("return", false, false, false));
    CHECK(s.stack.undo_count() == undo + 1);
    CHECK(s.top() == doctest::Approx(3.5f)); // 1 + 2.5
}

TEST_CASE("modeling tool: Ctrl+R loop-cuts the ring under the cursor; the wheel adds cuts") {
    Session s;
    REQUIRE(s.tool.key_pressed("r", true, false, false));
    CHECK(s.tool.busy());
    CHECK(s.tool.wheel(1.0f)); // two cuts
    CHECK(s.tool.loop_cuts == 2);
    // Hover the +X side, near its top edge, and click.
    s.tool.update({5.0f, 0.95f, 0.1f}, {-1.0f, 0.0f, 0.0f}, false, 1.0f);
    CHECK(s.tool.status().find("2 cuts") != std::string::npos);
    s.tool.update({5.0f, 0.95f, 0.1f}, {-1.0f, 0.0f, 0.0f}, true, 1.0f);
    CHECK_FALSE(s.tool.busy());
    CHECK(s.mesh().face_count() == 2 + 4 * 3); // a ring of four faces, each in three
    REQUIRE(s.tool.last_edit());
    REQUIRE(s.tool.adjust_last(0.5f, 1));
    CHECK(s.mesh().face_count() == 10);
}
