// SPDX-License-Identifier: Apache-2.0
// Layer and camera-bookmark operations (phases.md P2.5).
#include <doctest/doctest.h>

#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

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
    std::vector<ops::ChangeKind> events;

    Fixture() {
        ops::register_builtin_ops(registry);
        changes.subscribe([this](const ops::ChangeEvent& e) { events.push_back(e.kind); });
    }

    bool exec(const char* id, json params) { return stack.execute(registry.create(id, std::move(params))); }

    core::Guid entity(const char* name, const core::Guid& parent = {}) {
        const core::Guid guid = core::Guid::generate();
        json params = {{"name", name}, {"guid", guid.to_string()}};
        if (!parent.is_null()) {
            params["parent"] = parent.to_string();
        }
        REQUIRE(exec("entity.create", params));
        return guid;
    }
};

} // namespace

TEST_CASE("layers: every layer op round-trips through undo and redo") {
    Fixture f;
    const core::Guid a = f.entity("A");
    const core::Guid b = f.entity("B");
    const core::Guid layer{9, 1};
    const json start = scene::snapshot_world(f.world);

    std::vector<json> states{start};
    CHECK(f.exec("layer.create", {{"name", "Props"}, {"guid", layer.to_string()}}));
    states.push_back(scene::snapshot_world(f.world));
    CHECK(f.exec("layer.rename", {{"layer", layer.to_string()}, {"name", "Rocks"}}));
    states.push_back(scene::snapshot_world(f.world));
    CHECK(f.exec("layer.assign", {{"entities", {a.to_string(), b.to_string()}}, {"layer", layer.to_string()}}));
    states.push_back(scene::snapshot_world(f.world));
    CHECK(f.exec("layer.set_visible", {{"layer", layer.to_string()}, {"value", false}}));
    states.push_back(scene::snapshot_world(f.world));
    CHECK(f.exec("layer.set_locked", {{"layer", layer.to_string()}, {"value", true}}));
    states.push_back(scene::snapshot_world(f.world));
    CHECK(f.exec("layer.delete", {{"layer", layer.to_string()}}));
    states.push_back(scene::snapshot_world(f.world));

    CHECK(f.world.layers().size() == 0);
    CHECK(scene::layer_of(f.world, f.world.find(a)).is_null()); // back on the default layer

    for (std::size_t i = states.size() - 1; i-- > 0;) {
        REQUIRE(f.stack.undo());
        CHECK(scene::snapshot_world(f.world) == states[i]);
    }
    for (std::size_t i = 1; i < states.size(); ++i) {
        REQUIRE(f.stack.redo());
        CHECK(scene::snapshot_world(f.world) == states[i]);
    }
}

TEST_CASE("layers: visibility and lock are inherited down the hierarchy") {
    Fixture f;
    const core::Guid parent = f.entity("P");
    const core::Guid child = f.entity("C", parent);
    const core::Guid other = f.entity("O");
    const core::Guid layer{9, 2};
    REQUIRE(f.exec("layer.create", {{"name", "L"}, {"guid", layer.to_string()}}));
    REQUIRE(f.exec("layer.assign", {{"entities", {parent.to_string()}}, {"layer", layer.to_string()}}));

    const entt::entity c = f.world.find(child);
    const entt::entity o = f.world.find(other);
    CHECK(scene::is_visible(f.world, c));
    CHECK(scene::is_pickable(f.world, c));

    REQUIRE(f.exec("layer.set_locked", {{"layer", layer.to_string()}, {"value", true}}));
    CHECK(scene::is_visible(f.world, c));
    CHECK_FALSE(scene::is_pickable(f.world, c));
    CHECK(scene::is_pickable(f.world, o));

    REQUIRE(f.exec("layer.set_visible", {{"layer", layer.to_string()}, {"value", false}}));
    CHECK_FALSE(scene::is_visible(f.world, c));
    CHECK(scene::is_visible(f.world, o));
}

TEST_CASE("layers: invalid requests fail without changing anything") {
    Fixture f;
    const core::Guid a = f.entity("A");
    const std::size_t before = f.stack.undo_count();
    const core::Guid missing{9, 99};
    CHECK_FALSE(f.exec("layer.rename", {{"layer", missing.to_string()}, {"name", "x"}}));
    CHECK_FALSE(f.exec("layer.set_visible", {{"layer", missing.to_string()}, {"value", false}}));
    CHECK_FALSE(f.exec("layer.delete", {{"layer", missing.to_string()}}));
    CHECK_FALSE(f.exec("layer.assign", {{"entities", {a.to_string()}}, {"layer", missing.to_string()}}));
    const core::Guid layer{9, 3};
    REQUIRE(f.exec("layer.create", {{"name", "L"}, {"guid", layer.to_string()}}));
    CHECK_FALSE(f.exec("layer.create", {{"name", "dup"}, {"guid", layer.to_string()}})); // duplicate id
    CHECK(f.stack.undo_count() == before + 1);
    CHECK(f.events.back() == ops::ChangeKind::LayersChanged);
}

TEST_CASE("bookmarks: save/replace/delete undo, recall is transient") {
    Fixture f;
    json view = {{"name", "Overview"}, {"target", {1, 2, 3}}, {"distance", 20.0},
                 {"yaw", 0.5},         {"pitch", -0.3},       {"orthographic", false}};
    CHECK(f.exec("bookmark.save", view));
    REQUIRE(f.world.bookmarks().find("Overview") != nullptr);
    CHECK(f.world.bookmarks().find("Overview")->distance == doctest::Approx(20.0f));

    // Saving under the same name replaces; undo brings the old one back.
    view["distance"] = 5.0;
    CHECK(f.exec("bookmark.save", view));
    CHECK(f.world.bookmarks().bookmarks().size() == 1);
    CHECK(f.world.bookmarks().find("Overview")->distance == doctest::Approx(5.0f));
    f.stack.undo();
    CHECK(f.world.bookmarks().find("Overview")->distance == doctest::Approx(20.0f));

    // Recall: requests the view, records nothing, keeps the redo history.
    const std::size_t undo_before = f.stack.undo_count();
    CHECK(f.stack.can_redo());
    CHECK(f.exec("bookmark.recall", {{"name", "Overview"}}));
    CHECK(f.stack.undo_count() == undo_before);
    CHECK(f.stack.can_redo());
    REQUIRE(f.world.view_request().has_value());
    CHECK(f.world.view_request()->target == glm::vec3(1, 2, 3));
    CHECK(f.events.back() == ops::ChangeKind::ViewRequested);
    CHECK_FALSE(f.exec("bookmark.recall", {{"name", "Nope"}}));

    CHECK(f.exec("bookmark.delete", {{"name", "Overview"}}));
    CHECK(f.world.bookmarks().bookmarks().empty());
    f.stack.undo();
    CHECK(f.world.bookmarks().find("Overview") != nullptr);
}

TEST_CASE("snapshot: restore_world reproduces entities, layers and bookmarks") {
    Fixture f;
    const core::Guid a = f.entity("A");
    f.entity("B", a);
    const core::Guid layer{9, 4};
    REQUIRE(f.exec("layer.create", {{"name", "L"}, {"guid", layer.to_string()}}));
    REQUIRE(f.exec("layer.assign", {{"entities", {a.to_string()}}, {"layer", layer.to_string()}}));
    REQUIRE(f.exec("layer.set_locked", {{"layer", layer.to_string()}, {"value", true}}));
    REQUIRE(f.exec("bookmark.save", {{"name", "V"}, {"target", {0, 1, 0}}}));
    const json saved = scene::snapshot_world(f.world);

    scene::World copy;
    std::string error;
    REQUIRE(scene::restore_world(copy, saved, &error));
    CHECK(scene::snapshot_world(copy) == saved);
    CHECK_FALSE(scene::is_pickable(copy, copy.find(a)));

    CHECK_FALSE(scene::restore_world(copy, json::array(), &error));
    CHECK_FALSE(error.empty());
}
