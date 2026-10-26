// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"

#include <cmath>
#include <map>
#include <random>

using namespace f3d;
using namespace f3d::terrain;

namespace {

/// A heightfield of h(x, z) = f(x, z) in terrain-local metres.
template <class F>
Heightfield make(std::uint32_t tiles, float size, F f) {
    Heightfield hf(tiles, size);
    const float d = hf.spacing();
    for (std::uint32_t z = 0; z < hf.samples(); ++z) {
        for (std::uint32_t x = 0; x < hf.samples(); ++x) {
            hf.set(x, z, f(static_cast<float>(x) * d - size * 0.5f, static_cast<float>(z) * d - size * 0.5f));
        }
    }
    hf.update_bounds();
    return hf;
}

} // namespace

TEST_CASE("terrain: heightfield sizes, bilinear heights and normals") {
    const Heightfield slope = make(1, 256.0f, [](float x, float) { return 0.5f * x; });
    CHECK(slope.samples() == 257);
    CHECK(slope.spacing() == doctest::Approx(1.0f));
    CHECK(slope.height_at(10.3f, -4.0f) == doctest::Approx(5.15f).epsilon(1e-4));
    CHECK(slope.height_at(1000.0f, 0.0f) == doctest::Approx(64.0f)); // clamped to the edge
    const glm::vec3 n = slope.normal_at(0.0f, 0.0f);
    CHECK(n.x == doctest::Approx(-0.5f / std::sqrt(1.25f)).epsilon(1e-3));
    CHECK(n.y == doctest::Approx(1.0f / std::sqrt(1.25f)).epsilon(1e-3));
    CHECK(slope.range().x == doctest::Approx(-64.0f));
    CHECK(slope.range().y == doctest::Approx(64.0f));
    // A node's range bounds its own samples only.
    CHECK(slope.node_range(0, 0, kGridQuads).y == doctest::Approx(-48.0f));
}

TEST_CASE("terrain: raycasts hit the surface") {
    const Heightfield flat = make(1, 256.0f, [](float, float) { return 10.0f; });
    auto hit = raycast(flat, {3.0f, 100.0f, -7.0f}, {0.0f, -1.0f, 0.0f});
    REQUIRE(hit.has_value());
    CHECK(hit->y == doctest::Approx(10.0f).epsilon(1e-3));
    CHECK(hit->x == doctest::Approx(3.0f));
    CHECK_FALSE(raycast(flat, {3.0f, 100.0f, -7.0f}, {0.0f, 1.0f, 0.0f}).has_value());   // pointing away
    CHECK_FALSE(raycast(flat, {500.0f, 100.0f, 0.0f}, {0.0f, -1.0f, 0.0f}).has_value()); // off the terrain

    const Heightfield hills =
        make(2, 512.0f, [](float x, float z) { return 20.0f * std::sin(x * 0.05f) * std::cos(z * 0.04f); });
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> pos(-200.0f, 200.0f);
    for (int i = 0; i < 50; ++i) {
        const glm::vec3 origin(pos(rng), 80.0f, pos(rng));
        const glm::vec3 dir = glm::normalize(glm::vec3(pos(rng) * 0.002f, -1.0f, pos(rng) * 0.002f));
        const auto h = raycast(hills, origin, dir);
        REQUIRE(h.has_value());
        CHECK(std::abs(h->y - hills.height_at(h->x, h->z)) < 1e-2f);
    }
    // A grazing ray finds the first crossing, not a later one.
    const Heightfield bump = make(1, 256.0f, [](float x, float) { return x > 0.0f && x < 10.0f ? 5.0f : 0.0f; });
    const auto graze = raycast(bump, {-100.0f, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    REQUIRE(graze.has_value());
    CHECK(graze->x < 1.5f);
}

TEST_CASE("terrain: CDLOD selection covers the terrain once, LODs grow with distance, neighbours differ by 1") {
    Terrain t;
    t.tiles = 4;
    t.size = 1024.0f;
    const Heightfield hf = make(t.tiles, t.size, [](float x, float z) { return 0.02f * (x + z); });
    CHECK(lod_count(hf) == 6); // 1024 quads: 32 << 5

    for (const glm::vec3 camera :
         {glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(-480.0f, 5.0f, 300.0f), glm::vec3(2000.0f, 50.0f, 0.0f)}) {
        const LodSelection s = select_lods(hf, t, camera);
        // Coverage on a grid of finest cells (32 quads): every cell exactly once.
        const std::uint32_t cells = 1024 / kGridQuads;
        std::vector<int> count(cells * cells, 0);
        std::vector<std::uint32_t> lod_of(cells * cells, 0);
        for (const LodNode& n : s.nodes) {
            CHECK((n.quads == (kGridQuads << n.lod) || n.quads == (kGridQuads << n.lod) / 2));
            for (std::uint32_t z = n.z / kGridQuads; z < (n.z + n.quads) / kGridQuads; ++z) {
                for (std::uint32_t x = n.x / kGridQuads; x < (n.x + n.quads) / kGridQuads; ++x) {
                    ++count[z * cells + x];
                    lod_of[z * cells + x] = n.lod;
                }
            }
        }
        for (int c : count) {
            REQUIRE(c == 1);
        }
        // Adjacent cells: LODs differ by at most one (CDLOD morphing then closes every seam).
        for (std::uint32_t z = 0; z < cells; ++z) {
            for (std::uint32_t x = 0; x + 1 < cells; ++x) {
                CHECK(std::abs(int(lod_of[z * cells + x]) - int(lod_of[z * cells + x + 1])) <= 1);
                CHECK(std::abs(int(lod_of[x * cells + z]) - int(lod_of[(x + 1) * cells + z])) <= 1);
            }
        }
        if (camera.x == 0.0f) {
            // Under the camera: the finest LOD; the far corner: a coarse one.
            CHECK(lod_of[(cells / 2) * cells + cells / 2] == 0);
            CHECK(lod_of[0] >= 3);
        }
    }
    // More detail with a larger lod_distance.
    Terrain detailed = t;
    detailed.lod_distance = 6.0f;
    CHECK(select_lods(hf, detailed, {0, 30, 0}).nodes.size() > select_lods(hf, t, {0, 30, 0}).nodes.size());
}

TEST_CASE("terrain: frustum culling drops what is behind the camera") {
    Terrain t;
    const Heightfield hf = make(t.tiles, t.size, [](float, float) { return 0.0f; });
    const glm::vec3 eye(0.0f, 20.0f, 0.0f);
    const glm::mat4 vp = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 5000.0f) *
                         glm::lookAtRH(eye, eye + glm::vec3(1.0f, -0.2f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const auto planes = frustum_planes(vp);
    const LodSelection all = select_lods(hf, t, eye);
    const LodSelection seen = select_lods(hf, t, eye, &planes);
    CHECK(seen.nodes.size() < all.nodes.size());
    for (const LodNode& n : seen.nodes) {
        const float far_x = static_cast<float>(n.x + n.quads) * hf.spacing() - t.size * 0.5f;
        CHECK(far_x > eye.x - 1.0f); // nothing entirely behind (-x) the camera
    }
}

TEST_CASE("terrain: noise is deterministic and in range") {
    Heightfield a(2, 512.0f);
    Heightfield b(2, 512.0f);
    Heightfield c(2, 512.0f);
    generate_noise(a, {7}, 10.0f, 90.0f);
    generate_noise(b, {7}, 10.0f, 90.0f);
    generate_noise(c, {8}, 10.0f, 90.0f);
    CHECK(a.heights() == b.heights());
    CHECK(a.heights() != c.heights());
    const auto [lo, hi] = std::minmax_element(a.heights().begin(), a.heights().end());
    CHECK(*lo >= 10.0f);
    CHECK(*hi <= 90.0f);
    CHECK(*hi - *lo > 30.0f); // not flat
    CHECK(a.range().x == doctest::Approx(*lo));
    CHECK(a.range().y == doctest::Approx(*hi));
    CHECK(a.take_dirty_tiles().size() == 4);
    CHECK(a.take_dirty_tiles().empty());
    a.mark_dirty(256, 10, 256, 20); // on the edge between tiles 0 and 1
    CHECK(a.take_dirty_tiles() == std::vector<std::uint32_t>{0, 1});
}

TEST_CASE("terrain: terrain.create undoes and redoes, and the component is saved") {
    scene::register_types();
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    ops::register_builtin_ops(registry);
    register_terrain_ops(registry);

    const std::string guid = "00000000-0000-4000-8000-00000000b001";
    REQUIRE(stack.execute(registry.create(
        "terrain.create",
        {{"name", "Hills"}, {"guid", guid}, {"tiles", 1}, {"size", 256.0f}, {"max_height", 40.0f}, {"seed", 3}})));
    const entt::entity e = world.find(*core::Guid::parse(guid));
    REQUIRE((e != entt::null));
    const auto hf = heightfield_of(world, e);
    REQUIRE(hf != nullptr);
    CHECK(hf->samples() == 257);
    const std::vector<float> heights = hf->heights();
    const nlohmann::json snapshot = scene::snapshot_world(world);
    CHECK(snapshot["roots"][0]["components"]["Terrain"]["tiles"] == 1);

    REQUIRE(stack.undo());
    CHECK(world.size() == 0);
    CHECK(heightfields(world).find(*core::Guid::parse(guid)) == nullptr);
    REQUIRE(stack.redo());
    const entt::entity again = world.find(*core::Guid::parse(guid));
    REQUIRE((again != entt::null));
    CHECK(heightfield_of(world, again)->heights() == heights);

    CHECK_FALSE(stack.execute(registry.create("terrain.create", {{"tiles", 0}})));
    CHECK_FALSE(stack.execute(registry.create("terrain.create", {{"format", "r8"}})));
    // Terrains come from terrain.create, not entity.add_component.
    CHECK_FALSE(scene::is_addable_component("Terrain"));
}
