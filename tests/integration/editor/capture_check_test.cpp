// SPDX-License-Identifier: Apache-2.0
// Checks a PNG written by `forge3d-editor --capture` (ctest fixture
// integration.editor.capture.<rhi>) with the default camera: most pixels are
// the viewport clear colour between grid lines, and the red X and blue Z
// axis lines of the grid are visible.
#include <doctest/doctest.h>

#include "f3d/core/image.hpp"
#include "f3d/editor/frame_constants.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

TEST_CASE("editor capture: viewport shows the clear colour and the grid axes") {
    const char* path = std::getenv("F3D_CAPTURE_PNG");
    if (!path) {
        MESSAGE("F3D_CAPTURE_PNG not set; run through ctest integration.editor.capture.<rhi>.check");
        return;
    }
    const auto image = f3d::core::read_png(path);
    REQUIRE(image.has_value());
    REQUIRE(image->width > 0);

    // The viewport target is sRGB, so stored bytes are the encoded linear clear colour.
    int expected[3];
    for (int c = 0; c < 3; ++c) {
        expected[c] =
            static_cast<int>(std::lround(f3d::core::linear_to_srgb(f3d::editor::kViewportClearColor[c]) * 255.0f));
    }
    std::size_t background = 0;
    std::size_t red = 0;
    std::size_t blue = 0;
    for (std::uint32_t y = 0; y < image->height; ++y) {
        for (std::uint32_t x = 0; x < image->width; ++x) {
            const auto p = image->pixel(x, y);
            const int r = p[0], g = p[1], b = p[2];
            if (std::abs(r - expected[0]) <= 2 && std::abs(g - expected[1]) <= 2 && std::abs(b - expected[2]) <= 2) {
                ++background;
            }
            if (r > g + 50 && r > b + 50) {
                ++red;
            }
            if (b > r + 50 && b > g + 30) {
                ++blue;
            }
        }
    }
    const double total = static_cast<double>(image->width) * image->height;
    INFO("background ", background / total, ", red ", red, ", blue ", blue);
    CHECK(background / total > 0.4); // clear colour between the grid lines
    CHECK(red > image->width / 4);   // X axis line
    CHECK(blue > image->width / 4);  // Z axis line
}
