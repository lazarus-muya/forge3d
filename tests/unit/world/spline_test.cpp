// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/spline.hpp"
#include "f3d/world/world_ops.hpp"

#include <nlohmann/json.hpp>

#include <cmath>

using namespace f3d;

namespace {

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    core::Guid terrain = *core::Guid::parse("00000000-0000-4000-8000-00000000f001");
    core::Guid road = *core::Guid::parse("00000000-0000-4000-8000-00000000f002");
    core::Guid trees = *core::Guid::parse("00000000-0000-4000-8000-00000000f003");

    Session() {
        scene::register_types();
        ops::register_builtin_ops(registry);
        terrain::register_terrain_ops(registry);
        world::register_world_ops(registry);
        stack.set_limits(100000, 0);
        REQUIRE(run("terrain.create",
                    {{"guid", terrain.to_string()}, {"tiles", 2}, {"size", 512.0f}, {"max_height", 60.0f}, {"seed", 4}}));
    }
    bool run(const char* id, const nlohmann::json& params) { return stack.execute(registry.create(id, params)); }
    std::shared_ptr<terrain::Heightfield> authored() { return terrain::heightfield_of(world, world.find(terrain)); }
    std::shared_ptr<terrain::Heightfield> surface() { return terrain::surface_of(world, world.find(terrain)); }
    float ground(float x, float z) { return authored()->height_at(x, z); }
    nlohmann::json on_ground(const std::vector<glm::vec2>& xz) {
        nlohmann::json points = nlohmann::json::array();
        for (const glm::vec2& p : xz) {
            points.push_back({p.x, ground(p.x, p.y), p.y});
        }
        return points;
    }
    bool make_road() {
        return run("spline.create", {{"kind", "road"},
                                     {"guid", road.to_string()},
                                     {"terrain", terrain.to_string()},
                                     {"points", on_ground({{-150.0f, -40.0f}, {0.0f, 20.0f}, {150.0f, -10.0f}})}});
    }
};

} // namespace

TEST_CASE("spline: curves pass through their points") {
    const std::vector<glm::vec3> points = {{0, 0, 0}, {10, 2, 0}, {20, 0, 10}, {20, 5, 30}};
    const auto samples = world::evaluate_spline(points, false, 0.5f);
    REQUIRE(samples.size() > 40);
    for (const glm::vec3& p : points) {
        float best = 1e9f;
        for (const auto& s : samples) {
            best = std::min(best, glm::length(s.position - p));
        }
        CHECK(best < 1e-4f);
    }
    for (std::size_t i = 1; i < samples.size(); ++i) {
        CHECK(samples[i].distance > samples[i - 1].distance);
        CHECK(glm::length(samples[i].position - samples[i - 1].position) < 0.6f);
    }
    const auto loop = world::evaluate_spline(points, true, 0.5f);
    CHECK(glm::length(loop.back().position - points.front()) < 1e-4f);
    CHECK(world::evaluate_spline({{1, 2, 3}}, false).size() == 1);

    world::SplineShape shape(world::evaluate_spline({{0, 1, 0}, {100, 1, 0}}, false, 1.0f), 5.0f);
    const auto hit = shape.nearest(50.0f, 3.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->distance == doctest::Approx(3.0f));
    CHECK(hit->height == doctest::Approx(1.0f));
    CHECK(hit->along == doctest::Approx(50.0f).epsilon(0.01));
    CHECK_FALSE(shape.nearest(50.0f, 6.0f).has_value());
}

TEST_CASE("spline: a road shapes the surface, not the authored terrain, and undoes") {
    Session s;
    const std::string authored_before = terrain::height_hash(*s.authored());
    CHECK(s.surface() == s.authored()); // no splines: the authored terrain is the surface
    REQUIRE(s.make_road());
    world::update_landscape(s.world);
    REQUIRE(s.surface() != s.authored());
    CHECK(terrain::height_hash(*s.authored()) == authored_before);
    // On the road the ground follows the curve: across the road it is flat.
    const entt::entity e = s.world.find(s.road);
    const auto samples = world::evaluate_spline(world::spline_world_points(s.world, e), false, 1.0f);
    const auto& mid = samples[samples.size() / 2];
    glm::vec3 right = glm::normalize(glm::cross(mid.tangent, glm::vec3(0, 1, 0)));
    for (const float side : {-2.5f, 0.0f, 2.5f}) {
        const glm::vec3 p = mid.position + right * side;
        CHECK(s.surface()->height_at(p.x, p.z) == doctest::Approx(mid.position.y).epsilon(0.02));
    }
    // Far from it, the surface is the authored terrain.
    CHECK(s.surface()->height_at(0.0f, 200.0f) == s.authored()->height_at(0.0f, 200.0f));
    // It paints a dirt shoulder (a layer added for it).
    const world::Spline& road = s.world.registry().get<world::Spline>(e);
    REQUIRE(road.paint_layer >= 0);
    CHECK(s.world.registry().get<terrain::Terrain>(s.world.find(s.terrain)).layers[static_cast<std::size_t>(road.paint_layer)].name ==
          "Dirt");

    REQUIRE(s.stack.undo());
    world::update_landscape(s.world);
    CHECK(s.surface() == s.authored());
    CHECK(s.world.registry().get<terrain::Terrain>(s.world.find(s.terrain)).layers.size() == 3);
}

TEST_CASE("spline: move a point, undo: terrain and scatter are exactly as before") {
    Session s;
    REQUIRE(s.make_road());
    REQUIRE(s.run("entity.create", {{"name", "Trees"}, {"guid", s.trees.to_string()}}));
    REQUIRE(s.run("entity.add_component", {{"entity", s.trees.to_string()}, {"component", "Scatter"}}));
    const auto prop = [&](const char* field, const nlohmann::json& value) {
        REQUIRE(s.run("entity.set_property",
                      {{"entity", s.trees.to_string()}, {"path", std::string("Scatter.") + field}, {"value", value}}));
    };
    prop("meshes", {{{"guid", ""}, {"path", "tree"}}});
    prop("terrain", s.terrain.to_string());
    prop("area", {500.0f, 500.0f});
    prop("density", 0.02f);
    world::update_landscape(s.world);
    const std::string surface_before = terrain::height_hash(*s.surface());
    const auto trees_before = world::scatter_instances(s.world, s.world.find(s.trees));
    REQUIRE(trees_before.size() > 1000);
    // No tree on the road.
    const entt::entity e = s.world.find(s.road);
    const world::SplineShape shape(world::evaluate_spline(world::spline_world_points(s.world, e), false, 1.0f), 50.0f);
    for (const auto& t : trees_before) {
        const auto hit = shape.nearest(t.world[3].x, t.world[3].z);
        CHECK((!hit || hit->distance > 5.9f));
    }

    nlohmann::json moved = nlohmann::json::array();
    for (const glm::vec3& p : s.world.registry().get<world::Spline>(e).points) {
        moved.push_back({p.x, p.y, p.z});
    }
    moved[1][2] = moved[1][2].get<float>() + 40.0f;
    moved[1][1] = moved[1][1].get<float>() + 6.0f;
    REQUIRE(s.run("entity.set_property", {{"entity", s.road.to_string()}, {"path", "Spline.points"}, {"value", moved}}));
    world::update_landscape(s.world);
    CHECK(terrain::height_hash(*s.surface()) != surface_before);
    const auto trees_moved = world::scatter_instances(s.world, s.world.find(s.trees));
    CHECK(trees_moved.size() != trees_before.size());

    REQUIRE(s.stack.undo());
    world::update_landscape(s.world);
    CHECK(terrain::height_hash(*s.surface()) == surface_before);
    const auto trees_after = world::scatter_instances(s.world, s.world.find(s.trees));
    REQUIRE(trees_after.size() == trees_before.size());
    bool same = true;
    for (std::size_t i = 0; i < trees_after.size(); ++i) {
        same = same && trees_after[i].world == trees_before[i].world;
    }
    CHECK(same);
}

TEST_CASE("spline: sculpting under a road keeps the road and changes the rest") {
    Session s;
    REQUIRE(s.make_road());
    world::update_landscape(s.world);
    const entt::entity e = s.world.find(s.road);
    const auto samples = world::evaluate_spline(world::spline_world_points(s.world, e), false, 1.0f);
    const auto& mid = samples[samples.size() / 2];
    const float on_road = s.surface()->height_at(mid.position.x, mid.position.z);
    const glm::vec3 beside = mid.position + glm::vec3(0.0f, 0.0f, 25.0f);
    const float off_road = s.surface()->height_at(beside.x, beside.z);
    REQUIRE(s.run("terrain.sculpt_stroke", {{"terrain", s.terrain.to_string()},
                                            {"tool", "raise"},
                                            {"radius", 60.0f},
                                            {"strength", 1.0f},
                                            {"points", {{mid.position.x, mid.position.z, 1.0f}}}}));
    world::update_landscape(s.world);
    CHECK(s.surface()->height_at(mid.position.x, mid.position.z) == doctest::Approx(on_road).epsilon(0.001));
    CHECK(s.surface()->height_at(beside.x, beside.z) > off_road + 0.5f);
}

TEST_CASE("spline: geometry of roads, fences and rivers") {
    Session s;
    REQUIRE(s.make_road());
    world::update_landscape(s.world);
    const world::SplineGeometry& road = world::spline_geometry(s.world, s.world.find(s.road));
    REQUIRE(road.surface != nullptr);
    CHECK(road.surface->vertices.size() > 100);
    CHECK(road.surface->uvs.size() == road.surface->vertices.size());
    CHECK(road.props.empty());
    CHECK(road.water == nullptr);
    const std::uint64_t revision = road.revision;
    CHECK(world::spline_geometry(s.world, s.world.find(s.road)).revision == revision); // cached

    const core::Guid fence = core::Guid::generate();
    REQUIRE(s.run("spline.create", {{"kind", "fence"},
                                    {"guid", fence.to_string()},
                                    {"terrain", s.terrain.to_string()},
                                    {"points", s.on_ground({{0.0f, 100.0f}, {50.0f, 100.0f}})}}));
    world::update_landscape(s.world);
    const world::SplineGeometry& f = world::spline_geometry(s.world, s.world.find(fence));
    CHECK(f.surface == nullptr);
    // 50 m at 2.5 m: 21 posts and 20 gaps of two rails.
    CHECK(f.props.size() == 21 + 40);

    const core::Guid river = core::Guid::generate();
    REQUIRE(s.run("spline.create", {{"kind", "river"},
                                    {"guid", river.to_string()},
                                    {"terrain", s.terrain.to_string()},
                                    {"points", {{-200.0f, 10.0f, 150.0f}, {200.0f, 8.0f, 160.0f}}}}));
    world::update_landscape(s.world);
    const world::SplineGeometry& r = world::spline_geometry(s.world, s.world.find(river));
    REQUIRE(r.water != nullptr);
    CHECK(r.water->positions.size() == r.water->flow.size());
    CHECK(r.water->indices.size() % 3 == 0);
    CHECK(glm::length(r.water->flow[5]) == doctest::Approx(1.2f).epsilon(0.01));
    // The bed is carved below the water.
    CHECK(s.surface()->height_at(0.0f, 155.0f) < 9.0f - 2.0f);
    // Bad input.
    CHECK_FALSE(s.run("spline.create", {{"kind", "canal"}, {"points", {{0, 0, 0}}}}));
    CHECK_FALSE(s.run("spline.create", {{"kind", "road"}, {"points", nlohmann::json::array()}}));
}

TEST_CASE("spline: kinds and presets") {
    world::SplineKind kind{};
    CHECK(world::parse("river", kind));
    CHECK(kind == world::SplineKind::River);
    CHECK(world::to_string(world::SplineKind::Fence) == "fence");
    CHECK_FALSE(world::parse("canal", kind));
    CHECK(world::spline_preset(world::SplineKind::River).depth > 0.0f);
    CHECK_FALSE(world::spline_preset(world::SplineKind::Fence).shape_terrain);
    CHECK(world::spline_preset(world::SplineKind::Road).surface == "asphalt");
    CHECK(world::builtin_spline_meshes().size() == 2);
}
