// SPDX-License-Identifier: Apache-2.0
// The sculpt tool (phases.md P6.2): live strokes become one undoable operation each.
#include <doctest/doctest.h>

#include "f3d/editor/navigation.hpp"
#include "f3d/editor/sculpt_tool.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"

#include <nlohmann/json.hpp>

using namespace f3d;
using namespace f3d::editor;

namespace {

struct Scene {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    core::Guid terrain = *core::Guid::parse("00000000-0000-4000-8000-00000000e001");

    Scene() {
        scene::register_types();
        ops::register_builtin_ops(registry);
        terrain::register_terrain_ops(registry);
        REQUIRE(stack.execute(registry.create(
            "terrain.create",
            {{"guid", terrain.to_string()}, {"tiles", 1}, {"size", 256.0f}, {"max_height", 50.0f}, {"seed", 2}})));
    }
    [[nodiscard]] std::string hash() {
        return terrain::height_hash(*terrain::heightfield_of(world, world.find(terrain)));
    }
};

/// A ray straight down onto (x, z).
void at(SculptTool& tool, float x, float z, bool pressed, float pressure = 1.0f) {
    tool.update({x, 500.0f, z}, {0.0f, -1.0f, 0.0f}, pressed, pressure);
}

} // namespace

TEST_CASE("sculpt tool: a drag is one undo step; Escape restores") {
    Scene s;
    const std::string original = s.hash();
    SculptTool tool(s.world, s.stack, nullptr);
    tool.enabled = true;
    tool.brush.radius = 10.0f;
    tool.brush.strength = 1.0f;

    at(tool, -50.0f, 0.0f, false); // hovering: no stroke
    REQUIRE(tool.cursor().has_value());
    CHECK(tool.cursor()->x == doctest::Approx(-50.0f));
    CHECK_FALSE(tool.stroking());
    CHECK(s.hash() == original);

    for (int i = 0; i <= 10; ++i) {
        at(tool, -50.0f + 10.0f * static_cast<float>(i), 0.0f, true, 0.5f);
    }
    CHECK(tool.stroking());
    CHECK(s.hash() != original); // applied live
    CHECK(s.stack.undo_count() == 1);
    at(tool, 50.0f, 0.0f, false); // release
    CHECK_FALSE(tool.stroking());
    CHECK(tool.strokes() == 1);
    REQUIRE(s.stack.undo_count() == 2);
    CHECK(s.stack.undo_names().back() == "Sculpt (raise)");
    const std::string sculpted = s.hash();
    REQUIRE(s.stack.undo());
    CHECK(s.hash() == original);
    REQUIRE(s.stack.redo());
    CHECK(s.hash() == sculpted);

    // Escape mid-stroke: nothing recorded, heights as before.
    at(tool, 0.0f, 40.0f, true);
    at(tool, 30.0f, 40.0f, true);
    CHECK(s.hash() != sculpted);
    tool.cancel();
    CHECK(s.hash() == sculpted);
    at(tool, 30.0f, 40.0f, false);
    CHECK(s.stack.undo_count() == 2);

    // Off the terrain: no cursor, no stroke.
    tool.update({1000.0f, 500.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, true, 1.0f);
    CHECK_FALSE(tool.cursor().has_value());
    CHECK_FALSE(tool.stroking());
    tool.leave();
}

TEST_CASE("sculpt tool: brush settings from JSON; replays carry them") {
    Scene s;
    SculptTool tool(s.world, s.stack, nullptr);
    std::string error;
    CHECK(tool.apply_json({{"tool", "terrace"}, {"radius", 12.5}, {"falloff", "linear"}, {"terrace_step", 3}}, &error));
    CHECK(tool.brush.tool == terrain::BrushTool::Terrace);
    CHECK(tool.brush.radius == 12.5f);
    CHECK(tool.brush.falloff == terrain::BrushFalloff::Linear);
    CHECK(tool.brush.terrace_step == 3.0f);
    CHECK_FALSE(tool.apply_json({{"tool", "dig"}}, &error));
    CHECK(error.find("dig") != std::string::npos);
    CHECK(tool.brush.tool == terrain::BrushTool::Terrace); // unchanged on failure

    const ReplayResult replay = parse_replay(R"({"format": "forge3d.input_replay", "version": 1,
        "sculpt": {"tool": "smooth", "radius": 8}, "events": []})");
    REQUIRE(replay.replay);
    CHECK(nlohmann::json::parse(replay.replay->sculpt)["tool"] == "smooth");
    CHECK_FALSE(parse_replay(R"({"format": "forge3d.input_replay", "version": 1, "sculpt": 3, "events": []})").replay);
}
