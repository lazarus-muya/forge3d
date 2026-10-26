// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/guid.hpp"
#include "f3d/ops/entity_ops.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <random>
#include <string>
#include <vector>

using namespace f3d;
using nlohmann::json;

namespace {

struct Fixture {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;

    Fixture() {
        scene::register_types();
        ops::register_core_ops(registry);
        stack.set_limits(100000, 1ull << 40);
    }

    bool exec(const char* id, json params, bool merge = false) {
        return stack.execute(registry.create(id, std::move(params)), merge);
    }

    bool create(const std::string& name, const core::Guid& guid, const core::Guid& parent = {}) {
        json params = json::object({{"name", name}, {"guid", guid.to_string()}});
        if (!parent.is_null()) {
            params["parent"] = parent.to_string();
        }
        return exec("entity.create", std::move(params));
    }

    std::vector<core::Guid> live() const {
        std::vector<core::Guid> guids;
        for (const entt::entity e : world.depth_first()) {
            guids.push_back(world.guid(e));
        }
        return guids;
    }
};

json set_property_params(const core::Guid& entity, const char* path, json value) {
    return json::object({{"entity", entity.to_string()}, {"path", path}, {"value", std::move(value)}});
}

bool near(const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::abs(a[c][r] - b[c][r]) > eps * std::max(1.0f, std::abs(b[c][r]))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("ops: registry lists operations and builds them from JSON") {
    Fixture f;
    CHECK(f.registry.size() == 7);
    CHECK(f.registry.contains("entity.create"));
    CHECK_FALSE(f.registry.contains("does.not.exist"));
    CHECK(f.registry.create("does.not.exist", json::object()) == nullptr);

    const ops::OpInfo* info = f.registry.find("entity.set_property");
    REQUIRE(info != nullptr);
    CHECK(info->display_name == "Set Property");
    const json schema = ops::param_schema(info->params_type);
    REQUIRE(schema.is_array());
    CHECK(schema.size() == 2);
    CHECK(schema[0]["name"] == "entity");
    CHECK(schema[0]["type"] == "guid");
    CHECK(schema[1]["type"] == "string");

    auto op = f.registry.create("entity.create", json::object({{"name", "N"}}));
    REQUIRE(op != nullptr);
    CHECK(op->id() == "entity.create");
    CHECK(op->params_json()["name"] == "N");
}

TEST_CASE("ops: entity.create undoes and redoes") {
    Fixture f;
    const core::Guid g{1, 1};
    CHECK(f.create("A", g));
    CHECK((f.world.find(g) != entt::null));
    CHECK(f.stack.undo_count() == 1);

    f.stack.undo();
    CHECK((f.world.find(g) == entt::null));
    CHECK(f.stack.redo_count() == 1);

    f.stack.redo();
    CHECK((f.world.find(g) != entt::null));
}

TEST_CASE("ops: entity.delete restores the subtree exactly on undo") {
    Fixture f;
    const core::Guid a{1, 10};
    const core::Guid b{1, 11};
    const core::Guid c{1, 12};
    f.create("A", a);
    f.create("B", b, a);
    f.create("C", c, b);
    const entt::entity be = f.world.find(b);
    f.world.registry().emplace<scene::MeshRenderer>(be, scene::MeshRenderer{{core::Guid{}, "rock.glb#mesh/0"}, true});
    f.world.set_local(be, {1.0f, 2.0f, 3.0f}, glm::angleAxis(0.5f, glm::vec3(0, 1, 0)), {2.0f, 2.0f, 2.0f});

    const json before = scene::snapshot_world(f.world);
    CHECK(f.exec("entity.delete", json::object({{"entity", a.to_string()}})));
    CHECK((f.world.find(a) == entt::null));
    CHECK(f.world.size() == 0);

    f.stack.undo();
    CHECK(scene::snapshot_world(f.world) == before);
    CHECK((f.world.find(c) != entt::null));

    f.stack.redo();
    CHECK((f.world.find(a) == entt::null));
    CHECK(f.world.size() == 0);
}

TEST_CASE("ops: set_property merges a drag into one undo entry") {
    Fixture f;
    const core::Guid g{2, 1};
    f.create("E", g);
    const entt::entity e = f.world.find(g);
    const glm::vec3 original = f.world.transform(e).translation;
    CHECK(f.stack.undo_count() == 1); // the create

    // A drag: the first frame starts an entry, the following frames continue it.
    CHECK(f.exec("entity.set_property", set_property_params(g, "Transform.translation", json::array({1.0, 2.0, 3.0}))));
    CHECK(f.stack.undo_count() == 2);
    for (int frame = 1; frame <= 10; ++frame) {
        const double x = 1.0 + frame;
        CHECK(f.exec("entity.set_property", set_property_params(g, "Transform.translation", json::array({x, 5.0, 6.0})),
                     true));
    }
    CHECK(f.stack.undo_count() == 2); // merged: one entry for the whole drag
    CHECK(f.world.transform(e).translation == glm::vec3(11.0f, 5.0f, 6.0f));

    f.stack.undo();
    CHECK(f.world.transform(e).translation == original);
    f.stack.redo();
    CHECK(f.world.transform(e).translation == glm::vec3(11.0f, 5.0f, 6.0f));

    // A different path does not merge; the component is absent so it fails cleanly.
    CHECK_FALSE(f.exec("entity.set_property", set_property_params(g, "MeshRenderer.visible", true), true));
    // A malformed value fails cleanly instead of throwing.
    CHECK_FALSE(f.exec("entity.set_property", set_property_params(g, "Transform.translation", "nope")));
    CHECK(f.stack.undo_count() == 2);
}

TEST_CASE("ops: separate edits of the same property stay separate undo steps") {
    Fixture f;
    const core::Guid g{2, 2};
    f.create("E", g);
    CHECK(f.exec("entity.set_property", set_property_params(g, "Name.value", "first")));
    CHECK(f.exec("entity.set_property", set_property_params(g, "Name.value", "second")));
    CHECK(f.stack.undo_count() == 3);
    f.stack.undo();
    CHECK(f.world.name(f.world.find(g)) == "first");
}

TEST_CASE("ops: set_property on the transform updates the world matrix") {
    Fixture f;
    const core::Guid parent{2, 3};
    const core::Guid child{2, 4};
    f.create("P", parent);
    f.create("C", child, parent);
    const entt::entity c = f.world.find(child);
    CHECK(near(f.world.world_matrix(c), glm::mat4(1.0f)));

    CHECK(f.exec("entity.set_property", set_property_params(parent, "Transform.translation", json::array({1, 2, 3}))));
    CHECK(near(f.world.world_matrix(c), glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3))));
    f.stack.undo();
    CHECK(near(f.world.world_matrix(c), glm::mat4(1.0f)));
}

TEST_CASE("ops: merges inside a transaction fold into the matching child") {
    Fixture f;
    const core::Guid a{2, 5};
    const core::Guid b{2, 6};
    f.create("A", a);
    f.create("B", b);
    // A two-entity gizmo drag: one transaction, edits alternate between entities.
    f.stack.begin_transaction("Move");
    for (int frame = 0; frame < 5; ++frame) {
        const double x = frame + 1.0;
        CHECK(f.exec("entity.set_property", set_property_params(a, "Transform.translation", json::array({x, 0, 0})),
                     frame > 0));
        CHECK(f.exec("entity.set_property", set_property_params(b, "Transform.translation", json::array({0, x, 0})),
                     frame > 0));
    }
    f.stack.end_transaction();
    CHECK(f.stack.undo_count() == 3);
    f.stack.undo();
    CHECK(f.world.transform(f.world.find(a)).translation == glm::vec3(0.0f));
    CHECK(f.world.transform(f.world.find(b)).translation == glm::vec3(0.0f));
    f.stack.redo();
    CHECK(f.world.transform(f.world.find(a)).translation == glm::vec3(5.0f, 0.0f, 0.0f));
    CHECK(f.world.transform(f.world.find(b)).translation == glm::vec3(0.0f, 5.0f, 0.0f));
}

TEST_CASE("ops: entity.reparent keeps the world transform and undoes") {
    Fixture f;
    const core::Guid a{3, 1};
    const core::Guid b{3, 2};
    const core::Guid c{3, 3};
    f.create("A", a);
    f.create("B", b);
    f.create("C", c, a);
    const entt::entity ce = f.world.find(c);
    const glm::mat4 world_before = f.world.world_matrix(ce);

    CHECK(f.exec("entity.reparent", json::object({{"entity", c.to_string()}, {"parent", b.to_string()}})));
    CHECK((f.world.parent(ce) == f.world.find(b)));
    CHECK(near(f.world.world_matrix(ce), world_before));

    f.stack.undo();
    CHECK((f.world.parent(ce) == f.world.find(a)));
    CHECK(near(f.world.world_matrix(ce), world_before));
}

TEST_CASE("ops: entity.duplicate undoes and redoes") {
    Fixture f;
    const core::Guid a{4, 1};
    f.create("A", a);
    const json before = scene::snapshot_world(f.world);

    CHECK(f.exec("entity.duplicate", json::object({{"entity", a.to_string()}})));
    CHECK(f.world.size() == 2);
    const std::vector<core::Guid> live = f.live();
    REQUIRE(live.size() == 2);
    CHECK(live[0] != live[1]);

    f.stack.undo();
    CHECK(f.world.size() == 1);
    CHECK(scene::snapshot_world(f.world) == before);

    f.stack.redo();
    CHECK(f.world.size() == 2);
    CHECK(f.live() == live);
}

TEST_CASE("ops: the history cap evicts the oldest entries first") {
    Fixture f;
    f.stack.set_limits(3, 1ull << 40);
    std::vector<core::Guid> guids;
    for (int i = 0; i < 5; ++i) {
        const core::Guid g{5, static_cast<std::uint64_t>(i + 1)};
        guids.push_back(g);
        f.create("E" + std::to_string(i), g);
    }
    CHECK(f.stack.undo_count() == 3);
    const std::vector<std::string> names = f.stack.undo_names();
    CHECK(names == std::vector<std::string>{"Create E2", "Create E3", "Create E4"});

    while (f.stack.can_undo()) {
        f.stack.undo();
    }
    CHECK((f.world.find(guids[0]) != entt::null));
    CHECK((f.world.find(guids[1]) != entt::null));
    CHECK((f.world.find(guids[2]) == entt::null));
    CHECK((f.world.find(guids[3]) == entt::null));
    CHECK((f.world.find(guids[4]) == entt::null));
}

TEST_CASE("ops: the byte cap evicts the oldest entries but keeps the newest") {
    Fixture f;
    const core::Guid root{5, 100};
    f.create("Root", root);
    for (int i = 0; i < 20; ++i) {
        f.create("C" + std::to_string(i), core::Guid{5, static_cast<std::uint64_t>(200 + i)}, root);
    }
    // Deleting the root snapshots the whole subtree: by far the largest entry.
    CHECK(f.exec("entity.delete", json::object({{"entity", root.to_string()}})));
    const std::size_t before = f.stack.memory_bytes();
    f.stack.set_limits(1000, 1);
    CHECK(f.stack.undo_count() == 1);
    CHECK(f.stack.undo_names().back() == "Delete entity");
    CHECK(f.stack.memory_bytes() < before);
    f.stack.undo();
    CHECK(f.world.size() == 21);
}

TEST_CASE("ops: a transaction undoes and redoes as one entry") {
    Fixture f;
    const core::Guid a{8, 1};
    const core::Guid b{8, 2};
    const core::Guid c{8, 3};
    const core::Guid d{8, 4};

    f.stack.begin_transaction("Create two");
    f.create("A", a);
    f.create("B", b);
    f.stack.end_transaction();
    CHECK(f.stack.undo_count() == 1);
    CHECK(f.world.size() == 2);

    f.stack.undo();
    CHECK(f.world.size() == 0);
    f.stack.redo();
    CHECK(f.world.size() == 2);

    // Nested transactions commit into the outer one.
    f.stack.begin_transaction("outer");
    f.create("C", c);
    f.stack.begin_transaction("inner");
    f.create("D", d);
    f.stack.end_transaction();
    f.stack.end_transaction();
    CHECK(f.stack.undo_count() == 2);
    CHECK(f.world.size() == 4);
    f.stack.undo();
    CHECK(f.world.size() == 2);
}

TEST_CASE("ops: 1000 random operations undo to the start and redo to the end") {
    Fixture f;
    std::mt19937_64 rng(20261015);
    const auto rand_float = [&] { return static_cast<float>(rng() % 20000) * 0.001f - 10.0f; };

    for (int i = 0; i < 3; ++i) {
        f.create("seed" + std::to_string(i), core::Guid{6, static_cast<std::uint64_t>(i + 1)});
    }
    const json start = scene::snapshot_world(f.world);

    std::vector<json> pre_states; // state just before each recorded operation
    std::vector<std::string> descriptions;
    json state = start;
    for (int step = 0; step < 1000; ++step) {
        const std::vector<core::Guid> live = f.live();
        const int kind = static_cast<int>(rng() % 5);
        const std::size_t before = f.stack.undo_count();
        std::string description;
        if (kind == 0 || live.empty()) {
            const core::Guid parent = (live.empty() || rng() % 3 == 0) ? core::Guid{} : live[rng() % live.size()];
            f.create("n" + std::to_string(step), core::Guid{7, static_cast<std::uint64_t>(step) + 1}, parent);
            description = "create n" + std::to_string(step);
        } else if (kind == 1) {
            const core::Guid target = live[rng() % live.size()];
            f.exec("entity.delete", json::object({{"entity", target.to_string()}}));
            description = "delete " + target.to_string();
        } else if (kind == 2) {
            const core::Guid target = live[rng() % live.size()];
            f.exec("entity.set_property", set_property_params(target, "Transform.translation",
                                                              json::array({rand_float(), rand_float(), rand_float()})));
            description = "set " + target.to_string();
        } else if (kind == 3) {
            const core::Guid target = live[rng() % live.size()];
            json params = json::object({{"entity", target.to_string()}});
            if (rng() % 3 != 0) {
                params["parent"] = live[rng() % live.size()].to_string();
            }
            f.exec("entity.reparent", std::move(params));
            description = "reparent " + target.to_string();
        } else {
            const core::Guid target = live[rng() % live.size()];
            f.exec("entity.duplicate", json::object({{"entity", target.to_string()}}));
            description = "duplicate " + target.to_string();
        }
        if (f.stack.undo_count() > before) {
            pre_states.push_back(state);
            descriptions.push_back(description);
        }
        state = scene::snapshot_world(f.world);
    }
    const json end = state;

    for (std::size_t i = pre_states.size(); i-- > 0;) {
        f.stack.undo();
        if (scene::snapshot_world(f.world) != pre_states[i]) {
            MESSAGE("undo mismatch for ", descriptions[i]);
            CHECK(scene::snapshot_world(f.world) == pre_states[i]);
            break;
        }
    }
    CHECK(scene::snapshot_world(f.world) == start);

    while (f.stack.can_redo()) {
        f.stack.redo();
    }
    CHECK(scene::snapshot_world(f.world) == end);
}
