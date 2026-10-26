// SPDX-License-Identifier: Apache-2.0
// glTF import through operations, and the component ops it relies on.
#include <doctest/doctest.h>

#include "f3d/assets/mesh_library.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/script.hpp"
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

struct Fixture {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    Fixture() { ops::register_builtin_ops(registry); }
    bool exec(const char* id, json params) { return stack.execute(registry.create(id, std::move(params))); }
};

/// Names and mesh refs in depth-first order, with local transforms (comparable across worlds).
json structure(const scene::World& world) {
    json out = json::array();
    for (const entt::entity e : world.depth_first()) {
        const auto& t = world.transform(e);
        json row = {{"name", world.name(e)},
                    {"depth", world.depth(e)},
                    {"t", {t.translation.x, t.translation.y, t.translation.z}},
                    {"r", {t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z}},
                    {"s", {t.scale.x, t.scale.y, t.scale.z}}};
        if (const auto* mr = world.registry().try_get<scene::MeshRenderer>(e)) {
            row["mesh"] = mr->mesh.path;
        }
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace

TEST_CASE("import: a glTF imported through operations matches spawn_gltf and undoes in one step") {
    assets::MeshLibrary meshes;
    const fs::path file = fs::path(F3D_TEST_DATA_DIR) / "gltf" / "Fox.glb";
    std::string error;
    const auto gltf = meshes.load_file(file, &error);
    REQUIRE_MESSAGE(gltf, error);

    scene::World direct;
    assets::spawn_gltf(direct, *gltf, file);

    Fixture f;
    REQUIRE(f.exec("entity.create", {{"name", "Existing"}}));
    const json before = scene::snapshot_world(f.world);
    core::Guid root;
    const json script = assets::gltf_import_ops(*gltf, file, {}, root);
    const ops::ScriptResult r = ops::run_script(f.stack, f.registry, script, "Import Fox");
    REQUIRE_MESSAGE(r.ok, r.error);
    const entt::entity root_e = f.world.find(root);
    REQUIRE((root_e != entt::null));

    // Same hierarchy, transforms and mesh references as the direct spawn, after "Existing".
    scene::World imported_only;
    REQUIRE(scene::restore_world(imported_only, {{"roots", {scene::snapshot_subtree(f.world, root_e)}}}));
    CHECK(structure(imported_only) == structure(direct));

    CHECK(f.stack.undo_count() == 2);
    f.stack.undo();
    CHECK(scene::snapshot_world(f.world) == before);
    f.stack.redo();
    CHECK((f.world.find(root) != entt::null));
}

TEST_CASE("component ops: add and remove undo exactly") {
    Fixture f;
    const core::Guid e{4, 1};
    REQUIRE(f.exec("entity.create", {{"name", "E"}, {"guid", e.to_string()}}));
    const json start = scene::snapshot_world(f.world);

    CHECK(f.exec("entity.add_component",
                 {{"entity", e.to_string()}, {"component", "Light"}, {"value", {{"intensity", 7.5}, {"type", 2}}}}));
    const auto* light = f.world.registry().try_get<scene::Light>(f.world.find(e));
    REQUIRE(light != nullptr);
    CHECK(light->intensity == doctest::Approx(7.5f));
    CHECK(light->type == scene::LightType::Spot);
    const json with_light = scene::snapshot_world(f.world);

    CHECK_FALSE(f.exec("entity.add_component", {{"entity", e.to_string()}, {"component", "Light"}})); // already there
    CHECK_FALSE(f.exec("entity.add_component", {{"entity", e.to_string()}, {"component", "Transform"}}));
    CHECK_FALSE(f.exec("entity.add_component", {{"entity", e.to_string()}, {"component", "Nope"}}));
    CHECK_FALSE(f.exec("entity.remove_component", {{"entity", e.to_string()}, {"component", "Camera"}})); // absent

    CHECK(f.exec("entity.remove_component", {{"entity", e.to_string()}, {"component", "Light"}}));
    CHECK(scene::snapshot_world(f.world) == start);
    f.stack.undo(); // the light comes back with its values
    CHECK(scene::snapshot_world(f.world) == with_light);
    f.stack.undo();
    CHECK(scene::snapshot_world(f.world) == start);
}
