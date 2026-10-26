// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/erosion.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"

#include <cmath>
#include <cstring>
#include <numeric>

using namespace f3d;
using namespace f3d::terrain;

namespace {

/// A width x width grid of noisy hills (metres, 1 m apart).
std::vector<float> hills(std::uint32_t width, std::uint32_t seed = 3) {
    std::vector<float> h(static_cast<std::size_t>(width) * width);
    for (std::uint32_t z = 0; z < width; ++z) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width);
            const float v = static_cast<float>(z) / static_cast<float>(width);
            h[static_cast<std::size_t>(z) * width + x] =
                40.0f + 30.0f * gradient_noise(u * 4.0f, v * 4.0f, seed) + 8.0f * gradient_noise(u * 16.0f, v * 16.0f, seed + 1);
        }
    }
    return h;
}

bool identical(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

double total(const std::vector<float>& h) {
    return std::accumulate(h.begin(), h.end(), 0.0);
}

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    core::Guid terrain = *core::Guid::parse("00000000-0000-4000-8000-00000000d001");

    Session() {
        scene::register_types();
        ops::register_builtin_ops(registry);
        register_terrain_ops(registry);
        REQUIRE(stack.execute(registry.create(
            "terrain.create",
            {{"guid", terrain.to_string()}, {"tiles", 1}, {"size", 256.0f}, {"max_height", 120.0f}, {"seed", 7}})));
    }
    [[nodiscard]] std::shared_ptr<Heightfield> heights() { return heightfield_of(world, world.find(terrain)); }
    [[nodiscard]] std::string hash() { return height_hash(*heights()); }
};

} // namespace

TEST_CASE("erosion: modes parse") {
    ErosionMode mode{};
    CHECK(parse("thermal", mode));
    CHECK(mode == ErosionMode::Thermal);
    CHECK(to_string(ErosionMode::Both) == "both");
    CHECK_FALSE(parse("wind", mode));
    CHECK(erosion_memory(100, 100) == 100u * 100u * 4u * 11u);
}

TEST_CASE("erosion: deterministic for the same input and settings; the seed matters") {
    ErosionSettings settings;
    settings.iterations = 40;
    std::vector<float> a = hills(97);
    std::vector<float> b = a;
    std::vector<float> c = a;
    const std::vector<float> original = a;
    REQUIRE(erode(a, 97, 97, 1.0f, settings, 0.0f, 200.0f));
    REQUIRE(erode(b, 97, 97, 1.0f, settings, 0.0f, 200.0f));
    CHECK(identical(a, b));
    CHECK_FALSE(identical(a, original));
    settings.seed = 2;
    REQUIRE(erode(c, 97, 97, 1.0f, settings, 0.0f, 200.0f));
    CHECK_FALSE(identical(a, c));
}

TEST_CASE("erosion: thermal relaxes slopes steeper than the talus angle and keeps the mass") {
    constexpr std::uint32_t n = 33;
    std::vector<float> h(n * n, 10.0f);
    h[16 * n + 16] = 60.0f; // a needle
    const double mass = total(h);
    ErosionSettings settings;
    settings.mode = ErosionMode::Thermal;
    settings.iterations = 300;
    settings.talus_angle = 40.0f;
    REQUIRE(erode(h, n, n, 1.0f, settings, 0.0f, 100.0f));
    CHECK(total(h) == doctest::Approx(mass).epsilon(1e-5));
    CHECK(h[16 * n + 16] < 20.0f);
    float steepest = 0.0f;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x + 1 < n; ++x) {
            steepest = std::max(steepest, std::abs(h[z * n + x] - h[z * n + x + 1]));
        }
    }
    CHECK(steepest < std::tan(40.0f * 0.0174533f) * 1.6f); // close to the talus slope
}

TEST_CASE("erosion: hydraulic carries soil downhill") {
    constexpr std::uint32_t n = 65;
    std::vector<float> h(n * n);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            // A slope falling towards +z, with ripples for the water to find.
            h[z * n + x] = 64.0f - 0.6f * static_cast<float>(z) + 1.5f * std::sin(static_cast<float>(x) * 0.7f);
        }
    }
    const std::vector<float> before = h;
    ErosionSettings settings;
    settings.mode = ErosionMode::Hydraulic;
    settings.iterations = 150;
    REQUIRE(erode(h, n, n, 1.0f, settings, -100.0f, 200.0f));
    double top = 0.0, bottom = 0.0;
    for (std::uint32_t x = 0; x < n; ++x) {
        for (std::uint32_t z = 4; z < 20; ++z) {
            top += h[z * n + x] - before[z * n + x];
        }
        for (std::uint32_t z = n - 6; z < n; ++z) {
            bottom += h[z * n + x] - before[z * n + x];
        }
    }
    CHECK(top < 0.0);    // eroded upslope
    CHECK(bottom > 0.0); // deposited at the foot
}

TEST_CASE("erosion: cancelling leaves the heights unchanged") {
    std::vector<float> h = hills(65);
    const std::vector<float> original = h;
    ErosionSettings settings;
    settings.iterations = 100;
    float last = 0.0f;
    const bool done = erode(h, 65, 65, 1.0f, settings, 0.0f, 200.0f, [&](float fraction) {
        last = fraction;
        return fraction < 0.25f;
    });
    CHECK_FALSE(done);
    CHECK(last == doctest::Approx(0.25f));
    CHECK(identical(h, original));
}

TEST_CASE("erosion: terrain.erode undoes exactly, reports progress and can be cancelled") {
    Session session;
    const std::string original = session.hash();
    std::vector<float> fractions;
    session.stack.context().progress = [&](float f) {
        fractions.push_back(f);
        return true;
    };
    const nlohmann::json params = {{"terrain", session.terrain.to_string()}, {"iterations", 30}, {"seed", 4}};
    REQUIRE(session.stack.execute(session.registry.create("terrain.erode", params)));
    const std::string eroded = session.hash();
    CHECK(eroded != original);
    REQUIRE(fractions.size() == 30);
    CHECK(fractions.back() == doctest::Approx(1.0f));
    CHECK(session.stack.memory_bytes() > 0);

    REQUIRE(session.stack.undo());
    CHECK(session.hash() == original);
    REQUIRE(session.stack.redo());
    CHECK(session.hash() == eroded);

    // The same parameters on the same terrain give the same heights.
    Session again;
    REQUIRE(again.stack.execute(again.registry.create("terrain.erode", params)));
    CHECK(again.hash() == eroded);

    // A result computed elsewhere (the editor's worker) is used as is.
    Session precomputed;
    std::vector<float> result = precomputed.heights()->heights();
    ErosionSettings settings;
    settings.iterations = 30;
    settings.seed = 4;
    const Terrain& t = precomputed.world.registry().get<Terrain>(precomputed.world.find(precomputed.terrain));
    REQUIRE(erode(result, 257, 257, 1.0f, settings, t.min_height, t.max_height));
    REQUIRE(precomputed.stack.execute(
        make_erode_op(precomputed.terrain, settings, std::move(result), precomputed.heights()->revision())));
    CHECK(precomputed.hash() == eroded);

    // Cancelled: fails and changes nothing.
    REQUIRE(session.stack.undo());
    session.stack.context().progress = [](float f) { return f < 0.5f; };
    const std::size_t steps = session.stack.undo_count();
    CHECK_FALSE(session.stack.execute(session.registry.create("terrain.erode", params)));
    CHECK(session.hash() == original);
    CHECK(session.stack.undo_count() == steps);
    session.stack.context().progress = {};

    CHECK_FALSE(session.stack.execute(session.registry.create(
        "terrain.erode", {{"terrain", session.terrain.to_string()}, {"mode", "wind"}})));
    CHECK_FALSE(session.stack.execute(session.registry.create(
        "terrain.erode", {{"terrain", session.terrain.to_string()}, {"iterations", 0}})));
}

TEST_CASE("erosion: the erode brush works inside its radius only") {
    Session session;
    const std::vector<float> before = session.heights()->heights();
    REQUIRE(session.stack.execute(session.registry.create(
        "terrain.sculpt_stroke", {{"terrain", session.terrain.to_string()},
                                  {"tool", "erode"},
                                  {"radius", 20.0f},
                                  {"strength", 1.0f},
                                  {"erosion_iterations", 20},
                                  {"points", nlohmann::json::array({nlohmann::json::array({0.0f, 0.0f, 1.0f})})}})));
    const std::vector<float>& after = session.heights()->heights();
    CHECK_FALSE(identical(before, after));
    // Samples further than the radius from the centre (128, 128) are untouched.
    for (std::uint32_t z = 0; z < 257; z += 4) {
        for (std::uint32_t x = 0; x < 257; x += 4) {
            const float dx = static_cast<float>(x) - 128.0f;
            const float dz = static_cast<float>(z) - 128.0f;
            if (dx * dx + dz * dz > 21.0f * 21.0f) {
                REQUIRE(after[z * 257 + x] == before[z * 257 + x]);
            }
        }
    }
    REQUIRE(session.stack.undo());
    CHECK(identical(session.heights()->heights(), before));
}
