// SPDX-License-Identifier: Apache-2.0
// Prefabs (phases.md P3.4): instances follow their prefab and keep their overrides.
#include <doctest/doctest.h>

#include "f3d/core/vfs.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/prefab.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

using namespace f3d;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kLamp = "project://prefabs/lamp.f3dprefab";

struct Fixture {
    core::vfs::FileSystem files;
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes, &files};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    fs::path dir;

    explicit Fixture(const char* name) : dir(fs::path(F3D_TEST_OUTPUT_DIR) / "prefabs" / name) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
        ops::register_builtin_ops(registry);
        stack.set_limits(10000, 0);
    }

    bool exec(const char* id, json params) { return stack.execute(registry.create(id, std::move(params))); }

    core::Guid create(const char* name, const core::Guid& parent = {}) {
        const core::Guid guid = core::Guid::generate();
        json params = {{"name", name}, {"guid", guid.to_string()}};
        if (!parent.is_null()) {
            params["parent"] = parent.to_string();
        }
        REQUIRE(exec("entity.create", params));
        return guid;
    }
    void set(const core::Guid& e, const char* path, json value) {
        REQUIRE(exec("entity.set_property", {{"entity", e.to_string()}, {"path", path}, {"value", std::move(value)}}));
    }

    /// Lamp: a root with a Pole (Light) and a Bulb, turned into the prefab kLamp.
    core::Guid make_lamp() {
        const core::Guid lamp = create("Lamp");
        const core::Guid pole = create("Pole", lamp);
        create("Bulb", lamp);
        REQUIRE(exec("entity.add_component", {{"entity", pole.to_string()}, {"component", "Light"}}));
        set(pole, "Light.intensity", 2.0);
        REQUIRE(exec("prefab.create_from_selection", {{"entity", lamp.to_string()}, {"path", kLamp}}));
        return lamp;
    }

    core::Guid instantiate(const char* path, glm::vec3 position) {
        const core::Guid guid = core::Guid::generate();
        REQUIRE(
            exec("prefab.instantiate",
                 {{"prefab", path}, {"position", {position.x, position.y, position.z}}, {"guid", guid.to_string()}}));
        return guid;
    }

    /// The entity named `name` under the instance root `root`.
    entt::entity child(const core::Guid& root, const char* name) {
        for (const entt::entity e : world.subtree(world.find(root))) {
            if (world.name(e) == name) {
                return e;
            }
        }
        return entt::null;
    }
    const scene::Light& light(const core::Guid& root) {
        return world.registry().get<scene::Light>(child(root, "Pole"));
    }
    const scene::PrefabInstance& instance(const core::Guid& root) {
        return world.registry().get<scene::PrefabInstance>(world.find(root));
    }
};

} // namespace

TEST_CASE("prefab: create, instantiate and the file format") {
    Fixture f("create");
    const core::Guid lamp = f.make_lamp();
    REQUIRE(fs::exists(f.dir / "prefabs" / "lamp.f3dprefab"));
    const auto doc = ops::prefab::load(f.files, kLamp);
    REQUIRE(doc.has_value());
    CHECK(doc->at("root").at("children").size() == 2);
    CHECK(f.instance(lamp).prefab.path == kLamp); // the source became an instance

    const core::Guid b = f.instantiate(kLamp, {5, 0, 0});
    CHECK(f.world.transform(f.world.find(b)).translation == glm::vec3(5, 0, 0));
    REQUIRE((f.child(b, "Bulb") != entt::null));
    CHECK(f.light(b).intensity == doctest::Approx(2.0f));
    CHECK(ops::prefab::instances_of(f.world, kLamp).size() == 2);

    // Redo recreates the same entities (GUIDs included), so later operations still apply.
    const json after = scene::snapshot_world(f.world);
    f.stack.undo();
    CHECK((f.world.find(b) == entt::null));
    f.stack.redo();
    CHECK(scene::snapshot_world(f.world) == after);

    // Creating a prefab from part of an instance is refused (apply its overrides instead).
    CHECK_FALSE(f.exec("prefab.create_from_selection", {{"entity", f.world.guid(f.child(b, "Pole")).to_string()},
                                                        {"path", "project://prefabs/x.f3dprefab"}}));
}

TEST_CASE("prefab: editing the prefab updates every instance and keeps their overrides; undo works") {
    Fixture f("propagate");
    const core::Guid a = f.make_lamp();
    const core::Guid b = f.instantiate(kLamp, {5, 0, 0});
    const core::Guid c = f.instantiate(kLamp, {-5, 0, 0});

    // b overrides the pole's intensity: recorded on the instance.
    f.set(f.world.guid(f.child(b, "Pole")), "Light.intensity", 9.0);
    REQUIRE(f.instance(b).overrides.size() == 1);
    CHECK(f.instance(b).overrides[0].path == "Light.intensity");
    // Moving an instance (its root) isn't an override.
    f.set(c, "Transform.translation", json::array({-6, 0, 1}));
    CHECK(f.instance(c).overrides.empty());
    const json before = scene::snapshot_world(f.world);

    // Edit the prefab through instance a: new colour and intensity, a new child, Bulb removed.
    const entt::entity pole_a = f.child(a, "Pole");
    f.set(f.world.guid(pole_a), "Light.color", json::array({1.0, 0.5, 0.25}));
    f.set(f.world.guid(pole_a), "Light.intensity", 3.0);
    f.create("Shade", a);
    REQUIRE(f.exec("entity.delete", {{"entity", f.world.guid(f.child(a, "Bulb")).to_string()}}));
    REQUIRE(f.exec("prefab.apply_overrides", {{"entity", a.to_string()}}));
    CHECK(f.instance(a).overrides.empty());

    // Every instance follows; b keeps its overridden intensity; placements stay.
    CHECK(f.light(b).color == glm::vec3(1.0f, 0.5f, 0.25f));
    CHECK(f.light(c).color == glm::vec3(1.0f, 0.5f, 0.25f));
    CHECK(f.light(b).intensity == doctest::Approx(9.0f)); // override kept
    CHECK(f.light(c).intensity == doctest::Approx(3.0f)); // from the prefab
    for (const core::Guid& g : {b, c}) {
        CHECK((f.child(g, "Shade") != entt::null));
        CHECK((f.child(g, "Bulb") == entt::null));
    }
    CHECK(f.world.transform(f.world.find(b)).translation == glm::vec3(5, 0, 0));
    CHECK(f.world.transform(f.world.find(c)).translation == glm::vec3(-6, 0, 1));
    CHECK(f.instance(b).overrides.size() == 1);

    // One undo step reverts the prefab file and every instance.
    const json prefab_after = *ops::prefab::load(f.files, kLamp);
    f.stack.undo();
    CHECK(ops::prefab::load(f.files, kLamp)->at("root").at("children").size() == 2);
    for (int i = 0; i < 4; ++i) {
        f.stack.undo(); // the edits of a made before applying
    }
    CHECK(scene::snapshot_world(f.world) == before);
    for (int i = 0; i < 5; ++i) {
        f.stack.redo();
    }
    CHECK(*ops::prefab::load(f.files, kLamp) == prefab_after);
    CHECK(f.light(b).intensity == doctest::Approx(9.0f));
    CHECK(f.light(c).intensity == doctest::Approx(3.0f));
}

TEST_CASE("prefab: a prefab file changed outside the editor updates instances on request") {
    Fixture f("external");
    const core::Guid a = f.make_lamp();
    const core::Guid b = f.instantiate(kLamp, {5, 0, 0});
    f.set(f.world.guid(f.child(b, "Pole")), "Light.range", 50.0);

    json doc = *ops::prefab::load(f.files, kLamp);
    for (json& child : doc["root"]["children"]) {
        if (child["components"]["Name"]["value"] == "Pole") {
            child["components"]["Light"]["intensity"] = 7.0;
            child["components"]["Light"]["range"] = 1.0;
        }
    }
    REQUIRE(f.files.write_text(kLamp, doc.dump(2)));
    const json before = scene::snapshot_world(f.world);
    REQUIRE(f.exec("prefab.update_instances", {{"prefab", kLamp}}));
    CHECK(f.light(a).intensity == doctest::Approx(7.0f));
    CHECK(f.light(b).intensity == doctest::Approx(7.0f));
    CHECK(f.light(a).range == doctest::Approx(1.0f));
    CHECK(f.light(b).range == doctest::Approx(50.0f)); // override kept
    f.stack.undo();
    CHECK(scene::snapshot_world(f.world) == before);
    CHECK_FALSE(f.exec("prefab.update_instances", {{"prefab", "project://prefabs/none.f3dprefab"}}));
}

TEST_CASE("prefab: reverting overrides restores the prefab's values; undo restores the overrides") {
    Fixture f("revert");
    f.make_lamp();
    const core::Guid b = f.instantiate(kLamp, {5, 0, 0});
    const core::Guid pole = f.world.guid(f.child(b, "Pole"));
    f.set(pole, "Light.intensity", 9.0);
    f.set(pole, "Light.range", 3.0);
    REQUIRE(f.instance(b).overrides.size() == 2);

    // One property...
    REQUIRE(f.exec("prefab.revert_overrides",
                   {{"entity", b.to_string()},
                    {"node", f.world.registry().get<scene::PrefabLink>(f.world.find(pole)).node.to_string()},
                    {"path", "Light.range"}}));
    CHECK(f.light(b).range == doctest::Approx(10.0f));
    CHECK(f.light(b).intensity == doctest::Approx(9.0f));
    // ...then all.
    REQUIRE(f.exec("prefab.revert_overrides", {{"entity", b.to_string()}}));
    CHECK(f.light(b).intensity == doctest::Approx(2.0f));
    CHECK(f.instance(b).overrides.empty());
    CHECK_FALSE(f.exec("prefab.revert_overrides", {{"entity", b.to_string()}})); // nothing left
    f.stack.undo();
    CHECK(f.light(b).intensity == doctest::Approx(9.0f));
    CHECK(f.instance(b).overrides.size() == 1);

    // Undoing the edits themselves removes their overrides.
    f.stack.undo(); // the first revert: both overrides again
    CHECK(f.instance(b).overrides.size() == 2);
    f.stack.undo(); // the range edit
    CHECK(f.instance(b).overrides.size() == 1);
    f.stack.undo(); // the intensity edit
    CHECK(f.instance(b).overrides.empty());
    CHECK(f.light(b).intensity == doctest::Approx(2.0f));
}

TEST_CASE("prefab: one level of nesting") {
    Fixture f("nested");
    f.make_lamp();
    // A street corner: a sign plus a lamp instance, made into a prefab.
    const core::Guid corner = f.create("Corner");
    f.create("Sign", corner);
    const core::Guid inner = f.instantiate(kLamp, {1, 0, 0});
    REQUIRE(f.exec("entity.reparent", {{"entity", inner.to_string()}, {"parent", corner.to_string()}}));
    f.set(f.world.guid(f.child(inner, "Pole")), "Light.intensity", 4.0); // nested override, kept in the prefab
    const char* kCorner = "project://prefabs/corner.f3dprefab";
    REQUIRE(f.exec("prefab.create_from_selection", {{"entity", corner.to_string()}, {"path", kCorner}}));
    const auto doc = ops::prefab::load(f.files, kCorner);
    REQUIRE(doc.has_value());
    bool nested_ref = false;
    for (const json& child : doc->at("root").at("children")) {
        nested_ref |= child.contains("prefab") && child.at("prefab") == kLamp;
    }
    CHECK(nested_ref);

    // Instantiating the corner expands the lamp inside it, with its override.
    const core::Guid c2 = f.instantiate(kCorner, {20, 0, 0});
    const entt::entity lamp_in_c2 = f.child(c2, "Lamp");
    REQUIRE((lamp_in_c2 != entt::null));
    CHECK(f.world.registry().all_of<scene::PrefabInstance>(lamp_in_c2));
    CHECK(f.light(f.world.guid(lamp_in_c2)).intensity == doctest::Approx(4.0f));
    CHECK(f.world.transform(lamp_in_c2).translation.x == doctest::Approx(1.0f)); // placed by the corner

    // Changing the lamp prefab reaches the lamps inside corner instances (their override stays).
    json lamp_doc = *ops::prefab::load(f.files, kLamp);
    for (json& child : lamp_doc["root"]["children"]) {
        if (child["components"]["Name"]["value"] == "Pole") {
            child["components"]["Light"]["color"] = json::array({0.0, 1.0, 0.0});
            child["components"]["Light"]["intensity"] = 8.0;
        }
    }
    REQUIRE(f.files.write_text(kLamp, lamp_doc.dump(2)));
    REQUIRE(f.exec("prefab.update_instances", {{"prefab", kLamp}}));
    CHECK(f.light(f.world.guid(lamp_in_c2)).color == glm::vec3(0, 1, 0));
    CHECK(f.light(f.world.guid(lamp_in_c2)).intensity == doctest::Approx(4.0f));

    // Two levels are refused: a prefab containing a corner (which contains a lamp).
    const core::Guid district = f.create("District");
    REQUIRE(f.exec("entity.reparent", {{"entity", c2.to_string()}, {"parent", district.to_string()}}));
    REQUIRE(f.exec("prefab.create_from_selection",
                   {{"entity", district.to_string()}, {"path", "project://prefabs/district.f3dprefab"}}));
    CHECK_FALSE(f.exec("prefab.instantiate", {{"prefab", "project://prefabs/district.f3dprefab"}}));
}

TEST_CASE("prefab: instances survive a save/load round trip") {
    Fixture f("persist");
    f.make_lamp();
    const core::Guid b = f.instantiate(kLamp, {5, 0, 0});
    f.set(f.world.guid(f.child(b, "Pole")), "Light.intensity", 9.0);
    const json saved = scene::snapshot_world(f.world);
    scene::World loaded;
    REQUIRE(scene::restore_world(loaded, saved));
    CHECK(scene::snapshot_world(loaded) == saved);
    const auto& instance = loaded.registry().get<scene::PrefabInstance>(loaded.find(b));
    REQUIRE(instance.overrides.size() == 1);
    CHECK(json::parse(instance.overrides[0].value) == 9.0);
}
