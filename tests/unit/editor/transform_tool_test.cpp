// SPDX-License-Identifier: Apache-2.0
// Gizmo drags and selection combining (phases.md P2.4), without ImGuizmo: the
// tests feed TransformTool the matrices the gizmo would produce.
#include <doctest/doctest.h>

#include "f3d/editor/selection_tools.hpp"
#include "f3d/editor/transform_tool.hpp"
#include "f3d/ops/entity_ops.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/selection_ops.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <vector>

using namespace f3d;
using editor::GizmoOperation;
using editor::PivotMode;
using nlohmann::json;

namespace {

struct Fixture {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    editor::TransformTool tool{world, stack, registry};

    Fixture() {
        scene::register_types();
        ops::register_core_ops(registry);
        ops::register_selection_ops(registry);
    }

    core::Guid create(const char* name, const glm::vec3& position, const core::Guid& parent = {}) {
        const core::Guid guid = core::Guid::generate();
        json params = {{"name", name}, {"guid", guid.to_string()}};
        if (!parent.is_null()) {
            params["parent"] = parent.to_string();
        }
        REQUIRE(stack.execute(registry.create("entity.create", params)));
        REQUIRE(
            stack.execute(registry.create("entity.set_property", {{"entity", guid.to_string()},
                                                                  {"path", "Transform.translation"},
                                                                  {"value", {position.x, position.y, position.z}}})));
        return guid;
    }

    void select(const std::vector<core::Guid>& guids, const core::Guid& active = {}) {
        json list = json::array();
        for (const core::Guid& g : guids) {
            list.push_back(g.to_string());
        }
        REQUIRE(stack.execute(registry.create("selection.set", {{"entities", list}, {"active", active.to_string()}})));
    }

    glm::vec3 position(const core::Guid& guid) { return glm::vec3(world.world_matrix(world.find(guid))[3]); }
};

bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps)));
}

} // namespace

TEST_CASE("transform tool: a simulated gizmo drag produces exactly one undo entry") {
    Fixture f;
    const core::Guid a = f.create("A", {1, 0, 0});
    const core::Guid b = f.create("B", {3, 0, 0});
    f.select({a, b});
    const std::size_t before = f.stack.undo_count();

    const auto gizmo = f.tool.gizmo_matrix();
    REQUIRE(gizmo.has_value());
    CHECK(near(glm::vec3((*gizmo)[3]), {2, 0, 0})); // median of the two origins

    REQUIRE(f.tool.begin());
    for (int frame = 1; frame <= 30; ++frame) {
        glm::mat4 m = *gizmo;
        m[3] += glm::vec4(0.1f * frame, 0.05f * frame, 0.0f, 0.0f);
        f.tool.update(m);
    }
    f.tool.end();

    CHECK(f.stack.undo_count() == before + 1);
    CHECK(f.stack.undo_names().back() == "Move 2 entities");
    CHECK(near(f.position(a), {4.0f, 1.5f, 0.0f}));
    CHECK(near(f.position(b), {6.0f, 1.5f, 0.0f}));

    f.stack.undo();
    CHECK(near(f.position(a), {1, 0, 0}));
    CHECK(near(f.position(b), {3, 0, 0}));
    f.stack.redo();
    CHECK(near(f.position(a), {4.0f, 1.5f, 0.0f}));
}

TEST_CASE("transform tool: a click without movement records nothing") {
    Fixture f;
    const core::Guid a = f.create("A", {1, 0, 0});
    f.select({a});
    const std::size_t before = f.stack.undo_count();
    REQUIRE(f.tool.begin());
    f.tool.end();
    CHECK(f.stack.undo_count() == before);
}

TEST_CASE("transform tool: pivot modes for rotation") {
    const glm::mat4 quarter_turn = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));

    SUBCASE("median point: the selection rotates about its centre") {
        Fixture f;
        const core::Guid a = f.create("A", {1, 0, 0});
        const core::Guid b = f.create("B", {3, 0, 0});
        f.select({a, b});
        f.tool.operation = GizmoOperation::Rotate;
        const glm::mat4 gizmo = *f.tool.gizmo_matrix();
        REQUIRE(f.tool.begin());
        f.tool.update(gizmo * quarter_turn);
        f.tool.end();
        // About (2, 0, 0): +X offsets turn into -Z offsets.
        CHECK(near(f.position(a), {2, 0, 1}));
        CHECK(near(f.position(b), {2, 0, -1}));
    }
    SUBCASE("individual origins: each entity rotates in place") {
        Fixture f;
        const core::Guid a = f.create("A", {1, 0, 0});
        const core::Guid b = f.create("B", {3, 0, 0});
        f.select({a, b});
        f.tool.operation = GizmoOperation::Rotate;
        f.tool.pivot = PivotMode::IndividualOrigins;
        const glm::mat4 gizmo = *f.tool.gizmo_matrix();
        REQUIRE(f.tool.begin());
        f.tool.update(gizmo * quarter_turn);
        f.tool.end();
        CHECK(near(f.position(a), {1, 0, 0}));
        CHECK(near(f.position(b), {3, 0, 0}));
        const glm::mat4 world_a = f.world.world_matrix(f.world.find(a));
        CHECK(near(glm::vec3(world_a * glm::vec4(1, 0, 0, 0)), {0, 0, -1}));
    }
    SUBCASE("active element: rotates about the active entity") {
        Fixture f;
        const core::Guid a = f.create("A", {1, 0, 0});
        const core::Guid b = f.create("B", {3, 0, 0});
        f.select({a, b}, b);
        f.tool.operation = GizmoOperation::Rotate;
        f.tool.pivot = PivotMode::ActiveElement;
        const glm::mat4 gizmo = *f.tool.gizmo_matrix();
        CHECK(near(glm::vec3(gizmo[3]), {3, 0, 0}));
        REQUIRE(f.tool.begin());
        f.tool.update(gizmo * quarter_turn);
        f.tool.end();
        CHECK(near(f.position(a), {3, 0, 2}));
        CHECK(near(f.position(b), {3, 0, 0}));
    }
    SUBCASE("3D cursor: rotates about the cursor") {
        Fixture f;
        const core::Guid a = f.create("A", {1, 0, 0});
        f.select({a});
        f.tool.operation = GizmoOperation::Rotate;
        f.tool.pivot = PivotMode::Cursor;
        f.tool.cursor = {0, 0, 0};
        const glm::mat4 gizmo = *f.tool.gizmo_matrix();
        CHECK(near(glm::vec3(gizmo[3]), {0, 0, 0}));
        REQUIRE(f.tool.begin());
        f.tool.update(gizmo * quarter_turn);
        f.tool.end();
        CHECK(near(f.position(a), {0, 0, -1}));
    }
}

TEST_CASE("transform tool: local space orients the gizmo by the active entity") {
    Fixture f;
    const core::Guid a = f.create("A", {0, 0, 0});
    const glm::quat turned = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    REQUIRE(f.stack.execute(
        f.registry.create("entity.set_property", {{"entity", a.to_string()},
                                                  {"path", "Transform.rotation"},
                                                  {"value", {turned.w, turned.x, turned.y, turned.z}}})));
    f.select({a});
    f.tool.space = editor::GizmoSpace::Local;
    const glm::mat4 gizmo = *f.tool.gizmo_matrix();
    CHECK(near(glm::vec3(gizmo[0]), {0, 0, -1})); // local X points along world -Z
    f.tool.space = editor::GizmoSpace::World;
    CHECK(near(glm::vec3((*f.tool.gizmo_matrix())[0]), {1, 0, 0}));
}

TEST_CASE("transform tool: children of selected parents are not moved twice") {
    Fixture f;
    const core::Guid parent = f.create("P", {0, 0, 0});
    const core::Guid child = f.create("C", {1, 0, 0}, parent);
    f.select({parent, child});
    CHECK(f.tool.targets() == std::vector<core::Guid>{parent});

    const glm::mat4 gizmo = *f.tool.gizmo_matrix();
    REQUIRE(f.tool.begin());
    glm::mat4 moved = gizmo;
    moved[3] += glm::vec4(0, 0, 5, 0);
    f.tool.update(moved);
    f.tool.end();
    CHECK(near(f.position(parent), {0, 0, 5}));
    CHECK(near(f.position(child), {1, 0, 5}));
}

TEST_CASE("transform tool: cancel restores the start state") {
    Fixture f;
    const core::Guid a = f.create("A", {1, 2, 3});
    f.select({a});
    const std::size_t before = f.stack.undo_count();
    const glm::mat4 gizmo = *f.tool.gizmo_matrix();
    REQUIRE(f.tool.begin());
    glm::mat4 moved = gizmo;
    moved[3] = glm::vec4(9, 9, 9, 1);
    f.tool.update(moved);
    CHECK(near(f.position(a), {9, 9, 9}));
    f.tool.cancel();
    CHECK(near(f.position(a), {1, 2, 3}));
    CHECK(f.stack.undo_count() == before);
}

TEST_CASE("selection: combining picks with the current selection") {
    using editor::SelectMode;
    const core::Guid a{1, 1};
    const core::Guid b{1, 2};
    const core::Guid c{1, 3};
    const std::vector<core::Guid> current{a, b};

    auto r = editor::combine_selection(current, a, {c}, SelectMode::Replace);
    CHECK(r.entities == std::vector<core::Guid>{c});
    CHECK(r.active == c);

    r = editor::combine_selection(current, a, {}, SelectMode::Replace); // click on empty space
    CHECK(r.entities.empty());
    CHECK(r.active.is_null());

    r = editor::combine_selection(current, a, {b, c}, SelectMode::Add);
    CHECK(r.entities == std::vector<core::Guid>{a, b, c});
    CHECK(r.active == b);

    r = editor::combine_selection(current, a, {a}, SelectMode::Remove);
    CHECK(r.entities == std::vector<core::Guid>{b});
    CHECK(r.active == b);

    r = editor::combine_selection(current, a, {b}, SelectMode::Toggle); // shift-click a selected entity
    CHECK(r.entities == std::vector<core::Guid>{a});
    CHECK(r.active == a);
    r = editor::combine_selection(current, a, {c}, SelectMode::Toggle);
    CHECK(r.entities == std::vector<core::Guid>{a, b, c});
    CHECK(r.active == c);
}
