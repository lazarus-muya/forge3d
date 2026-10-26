// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/reflect.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"
#include "f3d/world/water.hpp"
#include "f3d/world/world_ops.hpp"

#include <entt/meta/meta.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace f3d;

namespace {

/// A 256 m heightfield (1 m spacing, local -128..128) shaped by `f(x, z)`.
template <class F>
terrain::Heightfield shaped(F f) {
    terrain::Heightfield hf(1, 256.0f);
    const float half = hf.size() * 0.5f;
    for (std::uint32_t z = 0; z < hf.samples(); ++z) {
        for (std::uint32_t x = 0; x < hf.samples(); ++x) {
            hf.set(x, z, f(static_cast<float>(x) * hf.spacing() - half, static_cast<float>(z) * hf.spacing() - half));
        }
    }
    hf.update_bounds();
    return hf;
}

bool wet_at(const world::FloodResult& flood, float x, float z) {
    const auto cx = static_cast<std::int64_t>(std::floor((x - flood.origin.x) / flood.step));
    const auto cz = static_cast<std::int64_t>(std::floor((z - flood.origin.y) / flood.step));
    if (cx < 0 || cz < 0 || cx >= flood.width || cz >= flood.height) {
        return false;
    }
    return flood.wet[static_cast<std::size_t>(cz) * flood.width + static_cast<std::size_t>(cx)] != 0;
}

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    core::Guid terrain = *core::Guid::parse("00000000-0000-4000-8000-00000000f101");
    core::Guid lake = *core::Guid::parse("00000000-0000-4000-8000-00000000f102");

    Session() {
        scene::register_types();
        ops::register_builtin_ops(registry);
        terrain::register_terrain_ops(registry);
        world::register_world_ops(registry);
        stack.set_limits(100000, 0);
        REQUIRE(run("terrain.create", {{"guid", terrain.to_string()}, {"tiles", 1}, {"size", 256.0f}}));
        // A bowl 0.01 r^2 deep (flat terrain.create ground replaced).
        terrain::Heightfield& hf = *terrain::heightfield_of(world, world.find(terrain));
        const float half = hf.size() * 0.5f;
        for (std::uint32_t z = 0; z < hf.samples(); ++z) {
            for (std::uint32_t x = 0; x < hf.samples(); ++x) {
                const float px = static_cast<float>(x) * hf.spacing() - half;
                const float pz = static_cast<float>(z) * hf.spacing() - half;
                hf.set(x, z, 0.01f * (px * px + pz * pz));
            }
        }
        hf.update_bounds();
        hf.mark_all_dirty();
    }
    bool run(const char* id, const nlohmann::json& params) { return stack.execute(registry.create(id, params)); }
    entt::entity lake_entity() { return world.find(lake); }
    bool make_lake(float level) {
        return run("water.create", {{"kind", "lake"},
                                    {"guid", lake.to_string()},
                                    {"terrain", terrain.to_string()},
                                    {"position", {0.0f, level, 0.0f}}});
    }
};

} // namespace

TEST_CASE("water: a bowl floods up to the level") {
    const auto bowl = shaped([](float x, float z) { return 0.01f * (x * x + z * z); });
    const world::FloodResult flood = world::flood_basin(bowl, {0.0f, 0.0f}, 4.0f, {256.0f, 256.0f});
    CHECK(flood.step == doctest::Approx(1.0f));
    // Ground below 4 m: a disc of radius 20 m.
    const float disc = std::numbers::pi_v<float> * 20.0f * 20.0f;
    CHECK(static_cast<float>(flood.cells) == doctest::Approx(disc).epsilon(0.08));
    CHECK(static_cast<std::size_t>(std::count(flood.wet.begin(), flood.wet.end(), 1)) == flood.cells);
    CHECK(wet_at(flood, 0.0f, 0.0f));
    CHECK(wet_at(flood, 15.0f, -10.0f));
    CHECK_FALSE(wet_at(flood, 25.0f, 0.0f));

    // A higher level floods more.
    const world::FloodResult higher = world::flood_basin(bowl, {0.0f, 0.0f}, 9.0f, {256.0f, 256.0f});
    CHECK(higher.cells > flood.cells);
    // The extent caps it.
    const world::FloodResult capped = world::flood_basin(bowl, {0.0f, 0.0f}, 9.0f, {20.0f, 20.0f});
    CHECK(capped.cells <= 20u * 20u);
    CHECK_FALSE(wet_at(capped, 15.0f, 0.0f));
    // max_cells grows the step to fit.
    const world::FloodResult coarse = world::flood_basin(bowl, {0.0f, 0.0f}, 9.0f, {256.0f, 256.0f}, 64);
    CHECK(coarse.step == doctest::Approx(4.0f));
    CHECK(coarse.width <= 64u);
}

TEST_CASE("water: a ridge keeps the next basin dry until the level tops it") {
    // Two bowls at x = -60 and x = +60, a 36 m ridge between them.
    const auto twin = shaped([](float x, float z) {
        const float a = (x + 60.0f) * (x + 60.0f) + z * z;
        const float b = (x - 60.0f) * (x - 60.0f) + z * z;
        return 0.01f * std::min(a, b);
    });
    const world::FloodResult low = world::flood_basin(twin, {-60.0f, 0.0f}, 10.0f, {256.0f, 256.0f});
    CHECK(low.cells > 0);
    CHECK(wet_at(low, -60.0f, 0.0f));
    CHECK_FALSE(wet_at(low, 60.0f, 0.0f));
    const world::FloodResult over = world::flood_basin(twin, {-60.0f, 0.0f}, 40.0f, {256.0f, 256.0f});
    CHECK(wet_at(over, 60.0f, 0.0f));
}

TEST_CASE("water: nothing floods when the seed is above the level") {
    const auto bowl = shaped([](float x, float z) { return 0.01f * (x * x + z * z); });
    const world::FloodResult dry = world::flood_basin(bowl, {50.0f, 0.0f}, 4.0f, {256.0f, 256.0f});
    CHECK(dry.cells == 0);
    CHECK(std::count(dry.wet.begin(), dry.wet.end(), 1) == 0);
    // A seed outside the terrain.
    CHECK(world::flood_basin(bowl, {500.0f, 0.0f}, 100.0f, {10.0f, 10.0f}).cells == 0);
}

TEST_CASE("water: a lake's surface follows its level and the ground") {
    Session s;
    REQUIRE(s.make_lake(4.0f));
    const entt::entity e = s.lake_entity();
    REQUIRE((e != entt::null));
    CHECK(world::water_level(s.world, e) == doctest::Approx(4.0f));
    const auto surface = world::lake_surface(s.world, e);
    REQUIRE(surface);
    REQUIRE(!surface->indices.empty());
    CHECK(surface->indices.size() % 6 == 0);
    for (const glm::vec3& p : surface->positions) {
        CHECK(p.y == doctest::Approx(4.0f));
    }
    // Cached while nothing changes.
    CHECK(world::lake_surface(s.world, e) == surface);

    // Raising the level floods more.
    REQUIRE(s.run("entity.set_property",
                  {{"entity", s.lake.to_string()}, {"path", "Transform.translation"}, {"value", {0.0f, 9.0f, 0.0f}}}));
    const auto raised = world::lake_surface(s.world, e);
    REQUIRE(raised);
    CHECK(raised->indices.size() > surface->indices.size());
    CHECK(raised->positions.front().y == doctest::Approx(9.0f));

    // Sculpting the ground above the level under the lake dries it.
    terrain::Heightfield& hf = *terrain::heightfield_of(s.world, s.world.find(s.terrain));
    std::fill(hf.heights().begin(), hf.heights().end(), 20.0f);
    hf.update_bounds();
    hf.mark_all_dirty();
    CHECK_FALSE(world::lake_surface(s.world, e));

    // Oceans and hidden lakes have no flooded surface.
    REQUIRE(s.run("entity.set_property", {{"entity", s.lake.to_string()},
                                          {"path", "WaterBody.kind"},
                                          {"value", static_cast<int>(world::WaterKind::Ocean)}}));
    CHECK_FALSE(world::lake_surface(s.world, e));
}

TEST_CASE("water: water.create is undoable") {
    Session s;
    REQUIRE(s.make_lake(5.0f));
    const entt::entity e = s.lake_entity();
    REQUIRE((e != entt::null));
    CHECK(s.world.name(e) == "Lake");
    const world::WaterBody& water = s.world.registry().get<world::WaterBody>(e);
    CHECK(water.kind == world::WaterKind::Lake);
    CHECK(water.terrain == s.terrain);

    REQUIRE(s.stack.undo());
    CHECK((s.lake_entity() == entt::null));
    REQUIRE(s.stack.redo());
    REQUIRE((s.lake_entity() != entt::null));
    CHECK(s.world.registry().all_of<world::WaterBody>(s.lake_entity()));

    // An ocean takes the ocean preset.
    REQUIRE(s.run("water.create", {{"kind", "ocean"}, {"position", {0.0f, 1.0f, 0.0f}}}));
    bool ocean = false;
    for (const entt::entity w : s.world.registry().view<world::WaterBody>()) {
        if (s.world.registry().get<world::WaterBody>(w).kind == world::WaterKind::Ocean) {
            ocean = true;
            CHECK(s.world.name(w) == "Ocean");
            CHECK(s.world.registry().get<world::WaterBody>(w) == world::water_preset(world::WaterKind::Ocean));
        }
    }
    CHECK(ocean);

    // Bad parameters fail without changing the scene.
    CHECK_FALSE(s.run("water.create", {{"kind", "pond"}}));
    CHECK_FALSE(s.run("water.create", {{"kind", "lake"}, {"terrain", core::Guid::generate().to_string()}}));
}

TEST_CASE("water: the component is reflected and round-trips through JSON") {
    world::register_water_types();
    const auto& types = scene::component_types();
    const auto it = std::find_if(types.begin(), types.end(), [](const auto& c) { return c.name == "WaterBody"; });
    REQUIRE(it != types.end());
    CHECK(it->addable);

    world::WaterBody w = world::water_preset(world::WaterKind::Ocean);
    w.terrain = core::Guid::generate();
    w.extent = {300.0f, 200.0f};
    w.visible = false;
    nlohmann::json json;
    core::reflect::value_to_json(json, entt::forward_as_meta(w));
    const entt::meta_any back = core::reflect::value_from_json(json, entt::resolve<world::WaterBody>());
    REQUIRE(back);
    CHECK(back.cast<const world::WaterBody&>() == w);

    world::WaterKind kind = world::WaterKind::Lake;
    CHECK(world::parse("ocean", kind));
    CHECK(kind == world::WaterKind::Ocean);
    CHECK(world::to_string(world::WaterKind::Lake) == "lake");
    CHECK_FALSE(world::parse("sea", kind));
    CHECK(world::water_preset(world::WaterKind::Ocean).wave_height >
          world::water_preset(world::WaterKind::Lake).wave_height);
}
