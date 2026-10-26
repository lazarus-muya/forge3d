// SPDX-License-Identifier: Apache-2.0
// The C API and plugin loading (phases.md P5.1): the sample plugin's operation is
// listed, runs, undoes and redoes exactly; a plugin built for another API version
// is refused before any of its code runs.
#include <doctest/doctest.h>

#include "f3d/api/host.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace f3d;

namespace {

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    api::Host host{registry, stack, world};

    Session() {
        scene::register_types();
        ops::register_builtin_ops(registry);
    }
};

} // namespace

TEST_CASE("api: version handshake") {
    const F3dApi* api = f3d_api_get(F3D_API_VERSION);
    REQUIRE(api != nullptr);
    CHECK(api->version == F3D_API_VERSION);
    CHECK(api->size == sizeof(F3dApi));
    CHECK(f3d_api_get((2u << 16)) == nullptr); // a newer major
    CHECK(std::string(api->last_error()).find("2.0") != std::string::npos);
    CHECK(f3d_api_get((1u << 16) | 7u) == nullptr); // a newer minor than this host
    CHECK(api::is_compatible(F3D_API_VERSION));
    CHECK_FALSE(api::is_compatible(0u << 16));
}

TEST_CASE("api: the sample plugin registers an undoable operation") {
    Session s;
    api::PluginLoader loader(s.host);
    const api::PluginRecord record = loader.load(F3D_HELLO_PLUGIN);
    REQUIRE_MESSAGE(record.loaded, record.error);
    CHECK(record.name == "hello_op");
    CHECK(record.version == "1.0.0");

    // Listed with its parameter schema.
    const ops::OpInfo* info = s.registry.find("sample.scatter_cubes");
    REQUIRE(info != nullptr);
    CHECK(info->display_name == "Scatter Cubes");
    const nlohmann::json schema = ops::param_schema(*info);
    REQUIRE(schema.size() == 4);
    CHECK(schema[0]["name"] == "count");
    CHECK(schema[2]["unit"] == "m");

    // Run through the C table, like an embedding host would.
    const F3dApi* api = f3d_api_get(F3D_API_VERSION);
    REQUIRE(api->run_op(s.host.handle(), "sample.scatter_cubes", R"({"count": 5, "seed": 3, "radius": 4})") == F3D_OK);
    F3dWorld* world = api->host_world(s.host.handle());
    CHECK(api->entity_count(world) == 6); // root + 5
    CHECK(s.stack.undo_count() == 1);     // one undo step
    F3dEntity cube{};
    REQUIRE(api->find_entity(world, "Cube 3", &cube) == F3D_OK);
    size_t needed = 0;
    CHECK(api->get_property(world, cube, "Transform.translation", nullptr, 0, &needed) == F3D_ERROR_BUFFER_TOO_SMALL);
    std::string text(needed, '\0');
    REQUIRE(api->get_property(world, cube, "Transform.translation", text.data(), text.size(), &needed) == F3D_OK);
    const nlohmann::json translation = nlohmann::json::parse(text.c_str());
    REQUIRE(translation.size() == 3);
    CHECK(std::abs(translation[0].get<float>()) <= 4.0f);
    CHECK(api->get_property(world, cube, "Transform.nope", text.data(), text.size(), &needed) == F3D_ERROR_NOT_FOUND);

    // Undo removes everything; redo replays the same scene (no new random draw).
    const nlohmann::json after = scene::snapshot_world(s.world);
    REQUIRE(api->undo(s.host.handle()) == F3D_OK);
    CHECK(api->entity_count(world) == 0);
    REQUIRE(api->redo(s.host.handle()) == F3D_OK);
    CHECK(scene::snapshot_world(s.world) == after);

    // The same seed gives the same layout in a fresh session.
    Session other;
    api::PluginLoader other_loader(other.host);
    REQUIRE(other_loader.load(F3D_HELLO_PLUGIN).loaded);
    REQUIRE(api->run_op(other.host.handle(), "sample.scatter_cubes", R"({"count": 5, "seed": 3, "radius": 4})") ==
            F3D_OK);
    F3dEntity other_cube{};
    REQUIRE(api->find_entity(api->host_world(other.host.handle()), "Cube 3", &other_cube) == F3D_OK);
    std::string other_text(needed, '\0');
    REQUIRE(api->get_property(api->host_world(other.host.handle()), other_cube, "Transform.translation",
                              other_text.data(), other_text.size(), &needed) == F3D_OK);
    CHECK(other_text == text);

    // A failing run (bad parameters) changes nothing.
    CHECK(api->run_op(s.host.handle(), "sample.scatter_cubes", R"({"count": -1})") == F3D_ERROR_FAILED);
    CHECK(scene::snapshot_world(s.world) == after);
    CHECK(s.stack.undo_count() == 1);

    // Loading it twice is refused: the operation id is taken.
    const api::PluginRecord again = loader.load(F3D_HELLO_PLUGIN);
    CHECK_FALSE(again.loaded);
    CHECK(again.error.find("duplicate") != std::string::npos);
}

TEST_CASE("api: a plugin built for another API version is refused with a clear error") {
    Session s;
    api::PluginLoader loader(s.host);
    const api::PluginRecord record = loader.load(F3D_FUTURE_PLUGIN);
    CHECK_FALSE(record.loaded);
    CHECK(record.error.find("API 2.0") != std::string::npos);
    CHECK(record.error.find("provides 1.0") != std::string::npos);
    CHECK(record.name == "f3d_test_future_api"); // the file name: its entry point never ran (it would rename itself)
    CHECK(s.registry.find("sample.scatter_cubes") == nullptr);

    const api::PluginRecord missing = loader.load("does-not-exist.dll");
    CHECK_FALSE(missing.loaded);
    CHECK_FALSE(missing.error.empty());
}

TEST_CASE("api: scene changes need an operation context") {
    const F3dApi* api = f3d_api_get(F3D_API_VERSION);
    F3dEntity out{};
    CHECK(api->create_entity(nullptr, "X", F3dEntity{0, 0}, &out) == F3D_ERROR_WRONG_CONTEXT);
    CHECK(api->set_property(nullptr, out, "Transform.translation", "[0,0,0]") == F3D_ERROR_WRONG_CONTEXT);
}
