// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/image.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/sculpt.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>

using namespace f3d;
using namespace f3d::terrain;

namespace {

bool bit_identical(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

/// A flat terrain at `height` (one tile per side unless given).
std::shared_ptr<Heightfield> flat(float height, std::uint32_t tiles = 1, float size = 256.0f) {
    auto hf = std::make_shared<Heightfield>(tiles, size);
    std::fill(hf->heights().begin(), hf->heights().end(), height);
    hf->update_bounds();
    return hf;
}

Terrain terrain_for(const Heightfield& hf, float min_height = -100.0f, float max_height = 100.0f) {
    Terrain t;
    t.tiles = hf.tiles();
    t.size = hf.size();
    t.min_height = min_height;
    t.max_height = max_height;
    return t;
}

/// The centre sample's height after one dab of `brush` at the origin.
float dab_at_centre(const std::shared_ptr<Heightfield>& hf, const Brush& brush,
                    std::shared_ptr<const Stamp> stamp = nullptr) {
    StrokeRecorder stroke(hf, terrain_for(*hf), brush, std::move(stamp));
    stroke.add({{0.0f, 0.0f}, 1.0f});
    return hf->at(hf->samples() / 2, hf->samples() / 2);
}

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    core::Guid terrain = *core::Guid::parse("00000000-0000-4000-8000-00000000c001");

    explicit Session(std::uint32_t tiles = 4, float size = 1024.0f) {
        scene::register_types();
        ops::register_builtin_ops(registry);
        register_terrain_ops(registry);
        stack.set_limits(100000, 0);
        REQUIRE(stack.execute(registry.create("terrain.create", {{"guid", terrain.to_string()},
                                                                 {"tiles", tiles},
                                                                 {"size", size},
                                                                 {"max_height", 300.0f},
                                                                 {"seed", 9}})));
    }
    [[nodiscard]] std::shared_ptr<Heightfield> heights() { return heightfield_of(world, world.find(terrain)); }
};

} // namespace

TEST_CASE("sculpt: falloff curves and names") {
    for (const BrushFalloff f : {BrushFalloff::Smooth, BrushFalloff::Linear, BrushFalloff::Sphere}) {
        CHECK(falloff_weight(f, 0.0f) == doctest::Approx(1.0f));
        CHECK(falloff_weight(f, 1.0f) == doctest::Approx(0.0f));
        CHECK(falloff_weight(f, 0.3f) > falloff_weight(f, 0.6f));
    }
    CHECK(falloff_weight(BrushFalloff::Constant, 0.9f) == 1.0f);
    BrushTool tool{};
    CHECK(parse("terrace", tool));
    CHECK(tool == BrushTool::Terrace);
    CHECK(to_string(BrushTool::Stamp) == "stamp");
    CHECK_FALSE(parse("dig", tool));
    BrushFalloff falloff{};
    CHECK(parse("sphere", falloff));
    CHECK(falloff == BrushFalloff::Sphere);
}

TEST_CASE("sculpt: dabs are evenly spaced and the same live or in one go") {
    const std::vector<StrokePoint> points = {
        {{0.0f, 0.0f}, 0.2f}, {{3.0f, 0.0f}, 0.4f}, {{3.5f, 0.0f}, 0.6f}, {{10.0f, 0.0f}, 1.0f}, {{10.0f, 4.0f}, 1.0f}};
    DabSpacer spacer(2.0f);
    std::vector<StrokePoint> dabs;
    for (const StrokePoint& p : points) {
        spacer.add(p, dabs);
    }
    // 14 m of polyline at 2 m: the first point plus 7 dabs.
    REQUIRE(dabs.size() == 8);
    CHECK(dabs[1].position.x == doctest::Approx(2.0f));
    CHECK(dabs[1].pressure == doctest::Approx(0.2f + 0.2f * 2.0f / 3.0f));
    CHECK(dabs[2].position.x == doctest::Approx(4.0f));
    CHECK(dabs[6].position.y == doctest::Approx(2.0f));
    // Recording a stroke point by point or replaying it gives identical heights.
    auto a = flat(10.0f);
    auto b = flat(10.0f);
    Brush brush;
    brush.radius = 12.0f;
    StrokeRecorder live(a, terrain_for(*a), brush);
    for (const StrokePoint& p : points) {
        live.add(p);
    }
    StrokeRecorder replay(b, terrain_for(*b), brush);
    for (const StrokePoint& p : live.points()) {
        replay.add(p);
    }
    CHECK(live.dab_count() == replay.dab_count());
    CHECK(bit_identical(a->heights(), b->heights()));
}

TEST_CASE("sculpt: each tool changes the heights the way it should") {
    Brush brush;
    brush.radius = 20.0f;
    brush.strength = 1.0f;

    brush.tool = BrushTool::Raise;
    CHECK(dab_at_centre(flat(10.0f), brush) == doctest::Approx(10.0f + 20.0f * 0.05f));
    brush.tool = BrushTool::Lower;
    CHECK(dab_at_centre(flat(10.0f), brush) == doctest::Approx(9.0f));

    // Outside the radius nothing changes; the rim barely moves (smooth falloff).
    {
        auto hf = flat(10.0f);
        brush.tool = BrushTool::Raise;
        StrokeRecorder stroke(hf, terrain_for(*hf), brush);
        stroke.add({{0.0f, 0.0f}, 1.0f});
        CHECK(hf->at(128 + 21, 128) == 10.0f);
        CHECK(hf->at(128 + 19, 128) < 10.01f);
        // Pressure scales the effect.
        auto soft = flat(10.0f);
        StrokeRecorder light(soft, terrain_for(*soft), brush);
        light.add({{0.0f, 0.0f}, 0.5f});
        CHECK(soft->at(128, 128) == doctest::Approx(10.5f));
    }

    brush.tool = BrushTool::Flatten;
    brush.use_target = true;
    brush.target_height = 4.0f;
    CHECK(dab_at_centre(flat(10.0f), brush) == doctest::Approx(4.0f));
    brush.use_target = false;

    brush.tool = BrushTool::Terrace;
    brush.terrace_step = 5.0f;
    CHECK(dab_at_centre(flat(11.0f), brush) == doctest::Approx(10.0f));
    CHECK(dab_at_centre(flat(13.0f), brush) == doctest::Approx(15.0f));

    // Smooth: a spike shrinks towards its neighbours.
    {
        auto hf = flat(0.0f);
        hf->set(128, 128, 50.0f);
        hf->update_bounds();
        brush.tool = BrushTool::Smooth;
        StrokeRecorder stroke(hf, terrain_for(*hf), brush);
        stroke.add({{0.0f, 0.0f}, 1.0f});
        CHECK(hf->at(128, 128) < 25.0f);
        CHECK(hf->at(129, 128) > 0.0f);
    }

    // Noise: deterministic, and it moves heights both ways.
    {
        brush.tool = BrushTool::Noise;
        brush.noise_scale = 4.0f;
        auto a = flat(10.0f);
        auto b = flat(10.0f);
        dab_at_centre(a, brush);
        dab_at_centre(b, brush);
        CHECK(bit_identical(a->heights(), b->heights()));
        const auto [lo, hi] = std::minmax_element(a->heights().begin(), a->heights().end());
        CHECK(*lo < 10.0f);
        CHECK(*hi > 10.0f);
    }

    // Stamp: white adds stamp_height (constant falloff), black adds nothing.
    {
        auto stamp = std::make_shared<Stamp>();
        stamp->width = 2;
        stamp->height = 1;
        stamp->values = {1.0f, 1.0f};
        brush.tool = BrushTool::Stamp;
        brush.falloff = BrushFalloff::Constant;
        brush.stamp_height = 6.0f;
        CHECK(dab_at_centre(flat(10.0f), brush, stamp) == doctest::Approx(16.0f));
        stamp->values = {0.0f, 0.0f};
        CHECK(dab_at_centre(flat(10.0f), brush, stamp) == doctest::Approx(10.0f));
    }

    // Heights stay inside the terrain's range.
    {
        auto hf = flat(99.5f);
        Brush big;
        big.radius = 20.0f;
        big.strength = 1.0f;
        StrokeRecorder stroke(hf, terrain_for(*hf), big);
        stroke.add({{0.0f, 0.0f}, 1.0f});
        CHECK(hf->at(128, 128) == 100.0f);
    }
}

TEST_CASE("sculpt: a stroke's diff covers the touched tiles only, and restores exactly") {
    auto hf = flat(5.0f, 4, 1024.0f);
    const std::vector<float> original = hf->heights();
    Brush brush;
    brush.radius = 10.0f;
    StrokeRecorder stroke(hf, terrain_for(*hf), brush);
    // Inside tile (0, 0), away from its edges: 1 tile.
    stroke.add({{-400.0f, -400.0f}, 1.0f});
    TileDiff diff = stroke.finish();
    REQUIRE(diff.tiles.size() == 1);
    CHECK(diff.tiles[0].index == 0);
    CHECK(diff.bytes() < 64 * 1024); // 257^2 floats = 264 KB raw
    const std::vector<float> after = hf->heights();
    diff.apply_before(*hf);
    CHECK(bit_identical(hf->heights(), original));
    diff.apply_after(*hf);
    CHECK(bit_identical(hf->heights(), after));
    // Across the corner shared by four tiles.
    StrokeRecorder corner(hf, terrain_for(*hf), brush);
    corner.add({{0.0f, 0.0f}, 1.0f});
    CHECK(corner.finish().tiles.size() == 4);
    corner.restore();
    CHECK(bit_identical(hf->heights(), after));
}

TEST_CASE("sculpt: 100 scripted strokes undo to bit-identical heights") {
    Session session;
    const std::vector<float> original = session.heights()->heights();
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pos(-560.0f, 560.0f); // some dabs hang over the edge
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const char* tools[] = {"raise", "lower", "smooth", "flatten", "terrace", "noise"};
    const char* falloffs[] = {"smooth", "linear", "sphere", "constant"};
    for (int i = 0; i < 100; ++i) {
        nlohmann::json points = nlohmann::json::array();
        glm::vec2 p{pos(rng), pos(rng)};
        for (int k = 0; k < 6; ++k) {
            points.push_back({p.x, p.y, 0.2f + 0.8f * unit(rng)});
            p += glm::vec2(unit(rng) - 0.5f, unit(rng) - 0.5f) * 40.0f;
        }
        const nlohmann::json params = {{"terrain", session.terrain.to_string()},
                                       {"tool", tools[i % 6]},
                                       {"falloff", falloffs[i % 4]},
                                       {"radius", 8.0f + 30.0f * unit(rng)},
                                       {"strength", unit(rng)},
                                       {"terrace_step", 6.0f},
                                       {"noise_scale", 10.0f},
                                       {"noise_seed", i},
                                       {"points", points}};
        REQUIRE(session.stack.execute(session.registry.create("terrain.sculpt_stroke", params)));
    }
    const std::vector<float> sculpted = session.heights()->heights();
    CHECK_FALSE(bit_identical(sculpted, original));
    CHECK(session.stack.memory_bytes() > 100 * sizeof(float));

    for (int i = 0; i < 100; ++i) {
        REQUIRE(session.stack.undo());
    }
    CHECK(bit_identical(session.heights()->heights(), original));
    for (int i = 0; i < 100; ++i) {
        REQUIRE(session.stack.redo());
    }
    CHECK(bit_identical(session.heights()->heights(), sculpted));
}

TEST_CASE("sculpt: a live stroke becomes an op that replays identically from its params") {
    Session live(1, 256.0f);
    Session replay(1, 256.0f);
    REQUIRE(bit_identical(live.heights()->heights(), replay.heights()->heights()));

    Brush brush;
    brush.tool = BrushTool::Flatten; // the target comes from the first dab
    brush.radius = 15.0f;
    brush.strength = 0.8f;
    const entt::entity e = live.world.find(live.terrain);
    auto stroke = std::make_unique<StrokeRecorder>(live.heights(), live.world.registry().get<Terrain>(e), brush);
    for (int i = 0; i < 20; ++i) {
        stroke->add({{-60.0f + 6.0f * static_cast<float>(i), 10.0f}, 0.5f + 0.02f * static_cast<float>(i)});
    }
    std::unique_ptr<ops::Operation> op = make_sculpt_stroke_op(live.terrain, std::move(stroke));
    const nlohmann::json params = op->params_json();
    CHECK(params["tool"] == "flatten");
    CHECK(params["points"].size() == 20);
    REQUIRE(live.stack.execute(std::move(op)));

    REQUIRE(replay.stack.execute(replay.registry.create("terrain.sculpt_stroke", params)));
    CHECK(bit_identical(live.heights()->heights(), replay.heights()->heights()));

    REQUIRE(live.stack.undo());
    CHECK_FALSE(bit_identical(live.heights()->heights(), replay.heights()->heights()));
    REQUIRE(replay.stack.undo());
    CHECK(bit_identical(live.heights()->heights(), replay.heights()->heights()));
}

TEST_CASE("sculpt: the stamp tool reads a heightmap image; bad params fail") {
    Session session(1, 256.0f);
    const std::filesystem::path dir = std::filesystem::path(F3D_TEST_OUTPUT_DIR) / "sculpt";
    std::filesystem::create_directories(dir);
    core::Image image;
    image.width = 8;
    image.height = 8;
    image.rgba.assign(8 * 8 * 4, 255);
    REQUIRE(core::write_png(dir / "white.png", image));
    const std::string guid = session.terrain.to_string();
    const nlohmann::json centre = nlohmann::json::array({nlohmann::json::array({0.0f, 0.0f, 1.0f})});
    const float before = session.heights()->at(128, 128);
    REQUIRE(session.stack.execute(session.registry.create(
        "terrain.sculpt_stroke", {{"terrain", guid},
                                  {"tool", "stamp"},
                                  {"falloff", "constant"},
                                  {"strength", 1.0f},
                                  {"radius", 20.0f},
                                  {"stamp", (dir / "white.png").string()},
                                  {"stamp_height", 7.0f},
                                  {"points", centre}})));
    CHECK(session.heights()->at(128, 128) == doctest::Approx(std::min(before + 7.0f, 300.0f)));

    const auto fails = [&](nlohmann::json params) {
        params["terrain"] = params.value("terrain", guid);
        return !session.stack.execute(session.registry.create("terrain.sculpt_stroke", params));
    };
    CHECK(fails({{"points", centre}, {"tool", "dig"}}));
    CHECK(fails({{"points", centre}, {"falloff", "steep"}}));
    CHECK(fails({{"points", nlohmann::json::array()}}));
    CHECK(fails({{"points", centre}, {"radius", 0.0f}}));
    CHECK(fails({{"points", centre}, {"tool", "stamp"}, {"stamp", "missing.png"}}));
    CHECK(fails({{"points", centre}, {"terrain", "00000000-0000-4000-8000-00000000dead"}}));
    CHECK(session.stack.undo_count() == 2); // terrain.create + the stamp
}
