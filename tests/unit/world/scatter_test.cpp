// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/reflect.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/world/scatter.hpp"

#include <entt/meta/meta.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

using namespace f3d;

namespace {

/// A heightfield of h(x, z) = f(x, z) in terrain-local metres.
template <class F>
terrain::Heightfield make(std::uint32_t tiles, float size, F f) {
    terrain::Heightfield hf(tiles, size);
    const float d = hf.spacing();
    for (std::uint32_t z = 0; z < hf.samples(); ++z) {
        for (std::uint32_t x = 0; x < hf.samples(); ++x) {
            hf.set(x, z, f(static_cast<float>(x) * d - size * 0.5f, static_cast<float>(z) * d - size * 0.5f));
        }
    }
    hf.update_bounds();
    return hf;
}

world::Scatter basic() {
    world::Scatter s;
    s.meshes = {{core::Guid{}, "a"}, {core::Guid{}, "b"}};
    s.area = {100.0f, 100.0f};
    s.density = 0.5f;
    s.min_spacing = 1.5f;
    return s;
}

glm::vec3 position(const world::ScatterInstance& i) {
    return glm::vec3(i.world[3]);
}

} // namespace

TEST_CASE("scatter: a fill is deterministic for a seed") {
    const world::Scatter s = basic();
    const auto a = world::generate_scatter(s, {10.0f, 2.0f, -5.0f}, {});
    const auto b = world::generate_scatter(s, {10.0f, 2.0f, -5.0f}, {});
    REQUIRE(a.size() > 100);
    REQUIRE(a.size() == b.size());
    bool same = true;
    for (std::size_t i = 0; i < a.size(); ++i) {
        same = same && a[i].world == b[i].world && a[i].mesh == b[i].mesh;
    }
    CHECK(same);

    world::Scatter other = s;
    other.seed = 2;
    const auto c = world::generate_scatter(other, {10.0f, 2.0f, -5.0f}, {});
    CHECK((c.size() != a.size() || c.front().world != a.front().world));

    // Both meshes are used; flat ground: on the entity's height, inside the area.
    CHECK(std::any_of(a.begin(), a.end(), [](const auto& i) { return i.mesh == 1; }));
    CHECK(std::all_of(a.begin(), a.end(), [](const auto& i) {
        const glm::vec3 p = position(i);
        return p.y == doctest::Approx(2.0f) && std::abs(p.x - 10.0f) <= 50.0f && std::abs(p.z + 5.0f) <= 50.0f;
    }));
}

TEST_CASE("scatter: no two instances closer than the minimum spacing") {
    world::Scatter s = basic();
    s.density = 4.0f; // far more candidates than fit
    s.min_spacing = 2.0f;
    s.sink = 0.3f;
    const auto instances = world::generate_scatter(s, {}, {});
    REQUIRE(instances.size() > 500);
    float closest = 1.0e9f;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        for (std::size_t j = i + 1; j < instances.size(); ++j) {
            const glm::vec3 d = position(instances[i]) - position(instances[j]);
            closest = std::min(closest, std::sqrt(d.x * d.x + d.z * d.z));
        }
    }
    CHECK(closest >= 2.0f);

    s.max_instances = 50;
    CHECK(world::generate_scatter(s, {}, {}).size() == 50);
    s.meshes.clear();
    CHECK(world::generate_scatter(s, {}, {}).empty());
}

TEST_CASE("scatter: slope, height and layer rules on a terrain") {
    // Flat at 0 m for x < 0, a 45-degree ramp for x > 0.
    terrain::Heightfield hf = make(1, 256.0f, [](float x, float) { return std::max(x, 0.0f); });
    terrain::Terrain t;
    t.tiles = 1;
    t.size = 256.0f;
    world::ScatterSurface surface{&hf, &t, glm::vec3(0.0f, 10.0f, 0.0f)};

    world::Scatter s = basic();
    s.area = {400.0f, 400.0f}; // larger than the terrain: what's off it is skipped
    s.min_spacing = 0.0f;
    s.density = 0.05f;
    s.slope_max = 20.0f;
    auto instances = world::generate_scatter(s, {}, surface);
    REQUIRE(!instances.empty());
    CHECK(std::all_of(instances.begin(), instances.end(), [](const auto& i) {
        const glm::vec3 p = position(i);
        return p.x < 1.0f && p.x >= -128.0f && std::abs(p.z) <= 128.0f && p.y == doctest::Approx(10.0f);
    }));

    s.slope_max = 90.0f;
    s.height_min = 60.0f; // only up the ramp: world height = 10 + x
    instances = world::generate_scatter(s, {}, surface);
    REQUIRE(!instances.empty());
    CHECK(std::all_of(instances.begin(), instances.end(), [](const auto& i) { return position(i).x >= 49.0f; }));

    // Layer 1 painted over z > 0 only.
    s.height_min = -1.0e4f;
    s.layer = 1;
    instances = world::generate_scatter(s, {}, surface); // no splat yet: everything is layer 0
    CHECK(instances.empty());
    terrain::Splatmap& splat = hf.ensure_splat(2);
    for (std::uint32_t z = splat.resolution() / 2; z < splat.resolution(); ++z) {
        for (std::uint32_t x = 0; x < splat.resolution(); ++x) {
            splat.set_weight(x, z, 0, 0);
            splat.set_weight(x, z, 1, 255);
        }
    }
    instances = world::generate_scatter(s, {}, surface);
    REQUIRE(!instances.empty());
    CHECK(std::all_of(instances.begin(), instances.end(), [](const auto& i) { return position(i).z >= -1.0f; }));

    // Aligned to the surface: the ramp tilts the instances' up axis by 45 degrees.
    s.layer = -1;
    s.height_min = 60.0f;
    s.align_to_normal = 1.0f;
    s.scale_min = s.scale_max = 1.0f;
    instances = world::generate_scatter(s, {}, surface);
    REQUIRE(!instances.empty());
    CHECK(glm::normalize(glm::vec3(instances.front().world[1])).y == doctest::Approx(std::sqrt(0.5f)).epsilon(0.02));
}

TEST_CASE("scatter: cached instances follow the component, the entity and the terrain") {
    world::register_types();
    scene::World w;
    const entt::entity e = w.create("Trees");
    world::Scatter s = basic();
    w.registry().emplace<world::Scatter>(e, s);
    const std::size_t count = world::scatter_instances(w, e).size();
    REQUIRE(count > 0);
    const glm::vec3 first = position(world::scatter_instances(w, e).front());

    w.registry().get<world::Scatter>(e).density = 0.1f;
    CHECK(world::scatter_instances(w, e).size() < count);
    w.registry().get<world::Scatter>(e).visible = false; // drawing only: same instances
    CHECK(world::scatter_instances(w, e).size() < count);

    w.registry().get<world::Scatter>(e) = s;
    w.set_local(e, {5.0f, 1.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    const glm::vec3 moved = position(world::scatter_instances(w, e).front());
    CHECK(moved.x == doctest::Approx(first.x + 5.0f));
    CHECK(moved.y == doctest::Approx(1.0f));

    // On a terrain: heights follow edits of the heightfield.
    const core::Guid tg = core::Guid::generate();
    const entt::entity t = w.create("Terrain", entt::null, tg);
    terrain::Terrain terrain;
    terrain.tiles = 1;
    terrain.size = 256.0f;
    w.registry().emplace<terrain::Terrain>(t, terrain);
    auto hf = std::make_shared<terrain::Heightfield>(make(1, 256.0f, [](float, float) { return 3.0f; }));
    terrain::heightfields(w).set(tg, hf);
    w.registry().get<world::Scatter>(e).terrain = tg;
    CHECK(position(world::scatter_instances(w, e).front()).y == doctest::Approx(3.0f));
    for (float& h : hf->heights()) {
        h = 7.0f;
    }
    hf->update_bounds();
    hf->mark_all_dirty(); // bumps the revision
    CHECK(position(world::scatter_instances(w, e).front()).y == doctest::Approx(7.0f));

    CHECK(world::scatter_instances(w, t).empty()); // not a scatter
    w.registry().remove<world::Scatter>(e);
    CHECK(world::scatter_instances(w, e).empty());
    world::prune_scatter_cache(w);
}

TEST_CASE("scatter: the component is reflected and round-trips through JSON") {
    world::register_types();
    const auto& types = scene::component_types();
    const auto it = std::find_if(types.begin(), types.end(), [](const auto& c) { return c.name == "Scatter"; });
    REQUIRE(it != types.end());
    CHECK(it->addable);

    world::Scatter s = basic();
    s.terrain = core::Guid::generate();
    s.layer = 2;
    nlohmann::json json;
    core::reflect::value_to_json(json, entt::forward_as_meta(s));
    CHECK(json["meshes"].size() == 2);
    const entt::meta_any back = core::reflect::value_from_json(json, entt::resolve<world::Scatter>());
    REQUIRE(back);
    CHECK(back.cast<const world::Scatter&>() == s);
}

TEST_CASE("scatter: built-in meshes and presets") {
    const auto meshes = world::builtin_scatter_meshes();
    REQUIRE(meshes.size() == 4);
    for (const auto& [path, mesh] : meshes) {
        CAPTURE(path);
        CHECK(!mesh.vertices.empty());
        CHECK(mesh.indices.size() % 3 == 0);
        CHECK(mesh.uvs.size() == mesh.vertices.size());
        CHECK(std::all_of(mesh.indices.begin(), mesh.indices.end(),
                          [&](std::uint32_t i) { return i < mesh.vertices.size(); }));
        CHECK(!mesh.bounds.empty());
        CHECK(mesh.bounds.max.y > 0.0f);
    }
    for (const world::ScatterPreset& preset : world::scatter_presets()) {
        CAPTURE(preset.id);
        for (const core::AssetRef& ref : preset.scatter.meshes) {
            CHECK(std::any_of(meshes.begin(), meshes.end(), [&](const auto& m) { return m.first == ref.path; }));
        }
    }
}
