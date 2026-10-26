// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/io/forge_file.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/scatter_paint.hpp"
#include "f3d/world/world_ops.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <random>

using namespace f3d;

namespace {

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    core::Guid scatter = *core::Guid::parse("00000000-0000-4000-8000-00000000e001");

    explicit Session(bool paint_only) {
        scene::register_types();
        ops::register_builtin_ops(registry);
        world::register_world_ops(registry);
        stack.set_limits(100000, 0);
        REQUIRE(run("entity.create", {{"name", "Scatter"}, {"guid", scatter.to_string()}}));
        REQUIRE(run("entity.add_component", {{"entity", scatter.to_string()}, {"component", "Scatter"}}));
        set("meshes", {{{"guid", ""}, {"path", "a"}}});
        set("area", {200.0f, 200.0f});
        set("density", 0.5f);
        set("min_spacing", 0.0f);
        set("paint_only", paint_only);
        stack.clear();
    }
    bool run(const char* id, const nlohmann::json& params) { return stack.execute(registry.create(id, params)); }
    void set(const char* field, const nlohmann::json& value) {
        REQUIRE(run("entity.set_property",
                    {{"entity", scatter.to_string()}, {"path", std::string("Scatter.") + field}, {"value", value}}));
    }
    std::size_t count() { return world::scatter_instances(world, world.find(scatter)).size(); }
    std::shared_ptr<world::ScatterMask> mask() { return world::scatter_mask_of(world, world.find(scatter)); }
    bool paint(float x, float z, float radius, bool erase) {
        return run("scatter.paint_stroke", {{"scatter", scatter.to_string()},
                                            {"erase", erase},
                                            {"radius", radius},
                                            {"strength", 1.0f},
                                            {"falloff", "constant"},
                                            {"points", {{x, z, 1.0f}}}});
    }
};

} // namespace

TEST_CASE("scatter paint: painted-only scatters grow where painted, undo removes them") {
    Session s(true);
    CHECK(s.count() == 0);
    CHECK(s.mask() == nullptr);
    REQUIRE(s.paint(30.0f, -20.0f, 10.0f, false));
    REQUIRE(s.mask() != nullptr);
    const auto& instances = world::scatter_instances(s.world, s.world.find(s.scatter));
    REQUIRE(instances.size() > 50); // ~0.5/m2 over ~314 m2
    for (const auto& i : instances) {
        const glm::vec2 p(i.world[3].x, i.world[3].z);
        CHECK(glm::length(p - glm::vec2(30.0f, -20.0f)) < 12.0f); // within the brush (and a cell)
    }
    const std::size_t painted = s.count();
    REQUIRE(s.stack.undo());
    CHECK(s.mask() == nullptr);
    CHECK(s.count() == 0);
    REQUIRE(s.stack.redo());
    CHECK(s.count() == painted);
}

TEST_CASE("scatter paint: erasing thins a filled scatter") {
    Session s(false);
    const std::size_t full = s.count();
    REQUIRE(full > 15000);
    REQUIRE(s.paint(0.0f, 0.0f, 40.0f, true));
    const std::size_t erased = s.count();
    CHECK(erased < full - 2000); // ~0.5/m2 over ~5000 m2
    for (const auto& i : world::scatter_instances(s.world, s.world.find(s.scatter))) {
        CHECK(glm::length(glm::vec2(i.world[3].x, i.world[3].z)) > 38.0f);
    }
    REQUIRE(s.stack.undo());
    CHECK(s.count() == full);
}

TEST_CASE("scatter paint: 100 strokes undo and redo the mask exactly, and it is saved") {
    Session s(false);
    REQUIRE(s.paint(0.0f, 0.0f, 5.0f, true)); // creates the mask
    const std::vector<std::uint8_t> first = s.mask()->cells();
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> pos(-110.0f, 110.0f);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(s.run("scatter.paint_stroke", {{"scatter", s.scatter.to_string()},
                                               {"erase", rng() % 2 == 0},
                                               {"radius", 3.0f + static_cast<float>(rng() % 30)},
                                               {"strength", 0.3f + 0.1f * static_cast<float>(rng() % 7)},
                                               {"points", {{pos(rng), pos(rng), 1.0f}, {pos(rng), pos(rng), 0.5f}}}}));
    }
    const std::vector<std::uint8_t> painted = s.mask()->cells();
    const std::size_t instances = s.count();
    for (int i = 0; i < 100; ++i) {
        REQUIRE(s.stack.undo());
    }
    CHECK(s.mask()->cells() == first);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(s.stack.redo());
    }
    CHECK(s.mask()->cells() == painted);

    const auto path = std::filesystem::temp_directory_path() / "f3d_scatter_paint_test.forge";
    REQUIRE(io::save_forge(path, s.world).ok);
    scene::World loaded;
    REQUIRE(io::load_forge(path, loaded).ok);
    const auto mask = world::scatter_mask_of(loaded, loaded.find(s.scatter));
    REQUIRE(mask != nullptr);
    CHECK(mask->cells() == painted);
    CHECK(world::scatter_instances(loaded, loaded.find(s.scatter)).size() == instances);
    std::filesystem::remove(path);
}

TEST_CASE("scatter paint: masks serialise and reject damage") {
    world::ScatterMask mask(3, 2, 7);
    mask.set(2, 1, 200);
    const auto bytes = mask.serialize();
    const auto back = world::ScatterMask::deserialize(bytes);
    REQUIRE(back.has_value());
    CHECK(back->width() == 3);
    CHECK(back->at(2, 1) == 200);
    auto damaged = bytes;
    damaged.pop_back();
    CHECK_FALSE(world::ScatterMask::deserialize(damaged).has_value());
    CHECK(world::mask_resolution({100.0f, 50.0f}) == glm::uvec2(100, 50));
    CHECK(world::mask_resolution({8192.0f, 8192.0f}) == glm::uvec2(2048, 2048));
    CHECK(mask.sample(0.0f, 0.0f) == doctest::Approx(7.0f / 255.0f));
}

TEST_CASE("scatter paint: bad strokes are refused") {
    Session s(true);
    CHECK_FALSE(s.run("scatter.paint_stroke", {{"scatter", s.scatter.to_string()}, {"points", nlohmann::json::array()}}));
    CHECK(s.mask() == nullptr);
    CHECK_FALSE(s.run("scatter.paint_stroke", {{"scatter", "00000000-0000-4000-8000-0000000000ff"},
                                               {"points", {{0.0f, 0.0f, 1.0f}}}}));
}
