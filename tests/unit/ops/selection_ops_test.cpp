// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/ops/entity_ops.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/selection_ops.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
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

    Fixture() {
        scene::register_types();
        ops::register_core_ops(registry);
        ops::register_selection_ops(registry);
    }

    bool exec(const char* id, json params) { return stack.execute(registry.create(id, std::move(params))); }
};

} // namespace

TEST_CASE("ops: selection set/add/remove round-trips through undo") {
    Fixture f;
    const core::Guid a{1, 1};
    const core::Guid b{1, 2};
    const core::Guid c{1, 3};
    for (const auto& [name, guid] : {std::pair{"A", a}, std::pair{"B", b}, std::pair{"C", c}}) {
        CHECK(f.exec("entity.create", json::object({{"name", name}, {"guid", guid.to_string()}})));
    }

    CHECK(f.exec("selection.set",
                 json::object({{"entities", json::array({a.to_string(), b.to_string()})}, {"active", a.to_string()}})));
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, b});
    CHECK(f.world.selection().active() == a);

    CHECK(f.exec("selection.add", json::object({{"entity", c.to_string()}})));
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, b, c});

    CHECK(f.exec("selection.remove", json::object({{"entity", b.to_string()}})));
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, c});

    f.stack.undo();
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, b, c});
    f.stack.undo();
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, b});
    f.stack.undo();
    CHECK(f.world.selection().empty());

    f.stack.redo();
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, b});
    f.stack.redo();
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, b, c});
    f.stack.redo();
    CHECK(f.world.selection().entities() == std::vector<core::Guid>{a, c});
}
