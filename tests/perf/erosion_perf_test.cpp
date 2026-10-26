// SPDX-License-Identifier: Apache-2.0
// phases.md P6.3: global erosion on a 4k² terrain (4097² samples, default settings:
// hydraulic + thermal, 200 iterations). Label `perf`; the time is printed for
// docs/budgets.md (target < 30 s on the reference machine). Run it in Release.
#include <doctest/doctest.h>

#include "f3d/terrain/erosion.hpp"
#include "f3d/terrain/terrain.hpp"

#include <chrono>
#include <cstdio>

using namespace f3d;
using namespace f3d::terrain;

TEST_CASE("perf: global erosion on 4k²") {
    Heightfield hf(16, 4096.0f); // 16 tiles x 256 quads
    NoiseSettings noise;
    noise.seed = 5;
    generate_noise(hf, noise, 0.0f, 600.0f);
    std::vector<float> heights = hf.heights();
    const ErosionSettings settings; // defaults: both, 200 iterations
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(erode(heights, hf.samples(), hf.samples(), hf.spacing(), settings, 0.0f, 600.0f));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("erosion 4097^2, %u iterations (%s): %.2f s, %.1f ms per iteration, %.0f MB scratch\n",
                settings.iterations, std::string(to_string(settings.mode)).c_str(), seconds,
                seconds * 1000.0 / settings.iterations,
                static_cast<double>(erosion_memory(hf.samples(), hf.samples())) / (1024.0 * 1024.0));
    CHECK(seconds < 120.0); // a regression guard; the budget itself is recorded, not asserted
}
