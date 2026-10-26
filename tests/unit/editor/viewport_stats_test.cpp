// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/viewport_stats.hpp"

#include <cmath>
#include <limits>

using namespace f3d::editor;

TEST_CASE("viewport stats: lines for frame rate, passes, draws and VRAM") {
    ViewportStats stats;
    stats.frame_ms = 8.0f;
    stats.gpu_ms = {{"frame", 3.0}, {"ui", 0.4}, {"viewport", 2.0}, {"mesh", 0.5}, {"shadows", 1.2}, {"sky", 0.1}};
    stats.draw_calls = 42;
    stats.local_lights = 3;
    stats.vram_used = 300ull * 1024 * 1024;
    stats.vram_budget = 4096ull * 1024 * 1024;
    stats.ev100 = 14.25f;
    const auto lines = format_viewport_stats(stats);
    REQUIRE(lines.size() == 8);
    CHECK(lines[0] == "125 fps  8.00 ms");
    CHECK(lines[1] == "GPU 2.00 ms");
    // Passes, most expensive first; frame and UI totals left out.
    CHECK(lines[2].find("shadows") != std::string::npos);
    CHECK(lines[3].find("mesh") != std::string::npos);
    CHECK(lines[4].find("sky") != std::string::npos);
    CHECK(lines[5] == "42 draws  3 lights");
    CHECK(lines[6] == "VRAM 300 / 4096 MB");
    CHECK(lines[7] == "EV100 14.2");

    stats.vram_used = 0;
    stats.ev100 = std::numeric_limits<float>::quiet_NaN();
    stats.gpu_ms.clear();
    CHECK(format_viewport_stats(stats).size() == 2); // fps, draws
}

TEST_CASE("viewport stats: frame time average") {
    FrameTimeAverage average;
    average.add(10.0f);
    CHECK(average.value() == 10.0f);
    for (int i = 0; i < 200; ++i) {
        average.add(20.0f);
    }
    CHECK(average.value() == doctest::Approx(20.0f).epsilon(0.001));
}
