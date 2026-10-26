// SPDX-License-Identifier: Apache-2.0
// Outliner / edit-command rules (editor::SceneEditor) and the measure tool (P2.5).
#include <doctest/doctest.h>

#include "f3d/editor/measure_tool.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <string>
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

    Fixture() { ops::register_builtin_ops(registry); }

    core::Guid make(const char* name, const core::Guid& parent = {}) {
        const core::Guid guid = editor.create_entity(name, parent);
        REQUIRE_FALSE(guid.is_null());
        return guid;
    }

    std::vector<std::string> names(const core::Guid& parent) {
        std::vector<std::string> out;
        for (const entt::entity e : world.children(parent.is_null() ? entt::null : world.find(parent))) {
            out.push_back(world.name(e));
        }
        return out;
    }
};

using Names = std::vector<std::string>;

} // namespace

TEST_CASE("scene editor: create_entity selects the new entity in one undo step") {
    Fixture f;
    const core::Guid a = f.make("A");
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a});
    CHECK(f.stack.undo_count() == 1);
    f.stack.undo();
    CHECK(f.world.size() == 0);
    CHECK(f.world.selection().empty());
}

TEST_CASE("scene editor: outliner moves (inside, before, after, root, cycles)") {
    Fixture f;
    const core::Guid a = f.make("A");
    const core::Guid b = f.make("B");
    const core::Guid c = f.make("C");
    const core::Guid d = f.make("D");
    CHECK(f.names({}) == Names{"A", "B", "C", "D"});

    SUBCASE("drop inside: appended as children, in drag order, one undo step") {
        const std::size_t before = f.stack.undo_count();
        CHECK(f.editor.move_entities({c, b}, a, {}));
        CHECK(f.names(a) == Names{"C", "B"});
        CHECK(f.names({}) == Names{"A", "D"});
        CHECK(f.stack.undo_count() == before + 1);
        f.stack.undo();
        CHECK(f.names({}) == Names{"A", "B", "C", "D"});
    }
    SUBCASE("drop before a sibling: reorders, accounting for the removed entities") {
        CHECK(f.editor.move_entities({a, b}, {}, d));
        CHECK(f.names({}) == Names{"C", "A", "B", "D"});
        CHECK(f.editor.move_entities({d}, {}, c));
        CHECK(f.names({}) == Names{"D", "C", "A", "B"});
    }
    SUBCASE("the anchor itself is being moved: insert before the next one that isn't") {
        CHECK(f.editor.move_entities({b, c}, {}, b));
        CHECK(f.names({}) == Names{"A", "B", "C", "D"});
    }
    SUBCASE("no cycles, and children of moved parents are not moved separately") {
        CHECK(f.editor.move_entities({b}, a, {}));
        CHECK(f.editor.move_entities({c}, b, {}));
        // Dropping A (an ancestor of B) into B is refused.
        CHECK_FALSE(f.editor.move_entities({a}, b, {}));
        // Moving A and its grandchild C to the root moves only A (C comes along).
        CHECK(f.editor.move_entities({a, c}, {}, {}));
        CHECK(f.names({}) == Names{"D", "A"});
        CHECK(f.names(b) == Names{"C"});
    }
}

TEST_CASE("scene editor: delete and duplicate act on the selected roots as one step") {
    Fixture f;
    const core::Guid a = f.make("A");
    const core::Guid child = f.make("Child", a);
    const core::Guid b = f.make("B");
    f.editor.set_selection({a, child, b});
    CHECK(f.editor.selected_roots() == std::vector<core::Guid>{a, b});
    const nlohmann::json before = scene::snapshot_world(f.world);
    const std::size_t steps = f.stack.undo_count();

    f.editor.duplicate_selection();
    CHECK(f.stack.undo_count() == steps + 1);
    CHECK(f.world.size() == 6); // plus copies of A (with its child) and B
    CHECK(f.world.selection().size() == 2);
    CHECK_FALSE(f.world.selection().contains(a));
    f.stack.undo();
    CHECK(scene::snapshot_world(f.world) == before);

    f.editor.set_selection({a, child, b});
    f.editor.delete_selection();
    CHECK(f.world.size() == 0);
    CHECK(f.world.selection().empty());
    f.stack.undo();
    CHECK(scene::snapshot_world(f.world) == before);
    CHECK(f.world.selection().size() == 3);
}

TEST_CASE("scene editor: rename, layers and bookmark names") {
    Fixture f;
    const core::Guid a = f.make("A");
    CHECK(f.editor.rename(a, "Rock"));
    CHECK(f.world.name(f.world.find(a)) == "Rock");
    CHECK_FALSE(f.editor.rename(a, "Rock")); // unchanged: no undo step

    const core::Guid l1 = f.editor.create_layer();
    const core::Guid l2 = f.editor.create_layer();
    CHECK(f.world.layers().find(l1)->name == "Layer 1");
    CHECK(f.world.layers().find(l2)->name == "Layer 2");
    f.editor.set_selection({a});
    CHECK(f.editor.assign_selection(l2));
    CHECK(f.editor.layer_members(l2) == std::vector<core::Guid>{a});
    CHECK(f.editor.layer_members({}).empty());

    scene::CameraBookmark view;
    CHECK(f.editor.save_bookmark(view) == "View 1");
    CHECK(f.editor.save_bookmark(view) == "View 2");
    CHECK(f.editor.recall_bookmark("View 1"));
    CHECK(f.world.view_request().has_value());
}

TEST_CASE("measure tool: distances, total and angle") {
    editor::MeasureTool m;
    CHECK_FALSE(m.segment(0).has_value());
    m.add({0, 0, 0});
    m.add({3, 0, 0});
    CHECK(*m.segment(0) == doctest::Approx(3.0f));
    CHECK_FALSE(m.angle_degrees().has_value());
    m.add({3, 4, 0});
    CHECK(*m.segment(1) == doctest::Approx(4.0f));
    CHECK(m.total() == doctest::Approx(7.0f));
    CHECK(*m.angle_degrees() == doctest::Approx(90.0f));
    // A fourth point starts over.
    m.add({1, 1, 1});
    CHECK(m.points().size() == 1);

    editor::MeasureTool straight;
    straight.add({0, 0, 0});
    straight.add({1, 0, 0});
    straight.add({2, 0, 0});
    CHECK(*straight.angle_degrees() == doctest::Approx(180.0f));
}
