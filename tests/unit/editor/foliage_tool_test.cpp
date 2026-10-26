// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/foliage_tool.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/world_ops.hpp"

#include <nlohmann/json.hpp>

using namespace f3d;

namespace {

/// A painted-only scatter on flat ground, and the brush as the viewport drives it.
struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    editor::FoliageTool tool{world, stack};
    core::Guid scatter = *core::Guid::parse("00000000-0000-4000-8000-00000000f001");

    Session() {
        scene::register_types();
        ops::register_builtin_ops(registry);
        world::register_world_ops(registry);
        REQUIRE(run("entity.create", {{"name", "Trees"}, {"guid", scatter.to_string()}}));
        REQUIRE(run("entity.add_component", {{"entity", scatter.to_string()}, {"component", "Scatter"}}));
        set("meshes", {{{"guid", ""}, {"path", "a"}}});
        set("area", {100.0f, 100.0f});
        set("density", 0.5f);
        set("min_spacing", 0.0f);
        set("paint_only", true);
        REQUIRE(run("selection.set", {{"entities", {scatter.to_string()}}}));
        stack.clear();
        tool.brush.radius = 8.0f;
        tool.brush.strength = 1.0f;
    }
    bool run(const char* id, const nlohmann::json& params) { return stack.execute(registry.create(id, params)); }
    void set(const char* field, const nlohmann::json& value) {
        REQUIRE(run("entity.set_property",
                    {{"entity", scatter.to_string()}, {"path", std::string("Scatter.") + field}, {"value", value}}));
    }
    std::size_t count() { return world::scatter_instances(world, world.find(scatter)).size(); }
    /// One frame of the cursor straight above (x, z), with the button held or not.
    void frame(float x, float z, bool pressed) { tool.update({x, 50.0f, z}, {0.0f, -1.0f, 0.0f}, pressed, 1.0f); }
};

} // namespace

TEST_CASE("foliage tool: a drag paints the selected scatter live, as one undo step") {
    Session s;
    CHECK(s.tool.current_scatter() == s.scatter);
    CHECK(s.count() == 0);
    s.frame(-20.0f, 0.0f, true);
    CHECK(s.tool.busy());
    CHECK(s.count() > 0); // live while dragging
    for (float x = -18.0f; x <= 20.0f; x += 2.0f) {
        s.frame(x, 0.0f, true);
    }
    const std::size_t painted = s.count();
    s.frame(20.0f, 0.0f, false);
    CHECK_FALSE(s.tool.busy());
    CHECK(s.tool.strokes() == 1);
    CHECK(s.count() == painted);
    for (const auto& i : world::scatter_instances(s.world, s.world.find(s.scatter))) {
        CHECK(std::abs(i.world[3].z) < 10.0f); // along the stroke
    }
    REQUIRE(s.stack.undo_count() == 1);
    REQUIRE(s.stack.undo());
    CHECK(s.count() == 0);
}

TEST_CASE("foliage tool: erase removes painted instances, cancel restores the stroke") {
    Session s;
    s.frame(0.0f, 0.0f, true);
    s.frame(0.0f, 0.0f, false);
    const std::size_t painted = s.count();
    REQUIRE(painted > 0);

    s.tool.erase_instances = true;
    s.frame(0.0f, 0.0f, true);
    CHECK(s.count() < painted);
    s.tool.cancel(); // Escape / right-click mid-stroke
    CHECK_FALSE(s.tool.busy());
    CHECK(s.count() == painted);

    s.frame(0.0f, 0.0f, true);
    s.frame(0.0f, 0.0f, false);
    CHECK(s.count() < painted);
}

TEST_CASE("foliage tool: nothing is painted without a scatter") {
    Session s;
    REQUIRE(s.run("selection.set", {{"entities", nlohmann::json::array()}}));
    CHECK(s.tool.current_scatter().is_null());
    s.frame(0.0f, 0.0f, true);
    CHECK_FALSE(s.tool.busy());
    s.frame(0.0f, 0.0f, false);
    CHECK(s.tool.strokes() == 0);
}
