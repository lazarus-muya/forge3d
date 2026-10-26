// SPDX-License-Identifier: Apache-2.0
// Operation scripts (forge3d.ops): --scene and forge3d-cli.
#include <doctest/doctest.h>

#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/script.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

using namespace f3d;
using nlohmann::json;

namespace {

struct Fixture {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    Fixture() { ops::register_builtin_ops(registry); }
};

json create(const char* name) {
    return {{"op", "entity.create"}, {"params", {{"name", name}}}};
}

} // namespace

TEST_CASE("script: runs every operation in order") {
    Fixture f;
    const json script = {{"format", "forge3d.ops"}, {"version", 1}, {"ops", {create("A"), create("B")}}};
    const ops::ScriptResult r = ops::run_script(f.stack, f.registry, script);
    CHECK(r.ok);
    CHECK(r.executed == 2);
    CHECK(f.world.size() == 2);
    CHECK(f.stack.undo_count() == 2);

    // A bare array works too.
    CHECK(ops::run_script(f.stack, f.registry, json::array({create("C")})).ok);
    CHECK(f.world.size() == 3);
}

TEST_CASE("script: reports the first failure") {
    Fixture f;
    ops::ScriptResult r =
        ops::run_script(f.stack, f.registry, {{"format", "other"}, {"version", 1}, {"ops", json::array()}});
    CHECK_FALSE(r.ok);
    r = ops::run_script(f.stack, f.registry, {{"format", "forge3d.ops"}, {"version", 2}, {"ops", json::array()}});
    CHECK_FALSE(r.ok);

    r = ops::run_script(f.stack, f.registry, json::array({create("A"), {{"op", "nope.nothing"}}, create("B")}));
    CHECK_FALSE(r.ok);
    CHECK(r.executed == 1);
    CHECK(r.error.find("op #2 nope.nothing") != std::string::npos);
    CHECK(f.world.size() == 1); // stopped at the failure

    r = ops::run_script(f.stack, f.registry,
                        json::array({{{"op", "entity.delete"}, {"params", {{"entity", "not-a-guid"}}}}}));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("bad parameters") != std::string::npos);
}

TEST_CASE("script: in a transaction it is all or nothing, one undo step") {
    Fixture f;
    ops::ScriptResult r = ops::run_script(f.stack, f.registry, json::array({create("A"), create("B")}), "Setup");
    CHECK(r.ok);
    CHECK(f.stack.undo_count() == 1);
    CHECK(f.stack.undo_names().back() == "Setup");

    r = ops::run_script(f.stack, f.registry, json::array({create("C"), {{"op", "bad"}}}), "Broken");
    CHECK_FALSE(r.ok);
    CHECK(f.world.size() == 2); // C was rolled back
    CHECK(f.stack.undo_count() == 1);
}

TEST_CASE("script: the committed outliner scene runs") {
    Fixture f;
    const ops::ScriptResult r = ops::run_script_file(
        f.stack, f.registry, std::filesystem::path(F3D_TEST_DATA_DIR) / "scenes" / "outliner_layers.json");
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(f.world.layers().size() == 3);
    CHECK(f.world.bookmarks().find("Overview") != nullptr);
    CHECK(f.world.selection().size() == 2);
    CHECK_FALSE(ops::run_script_file(f.stack, f.registry, "does/not/exist.json").ok);
}
