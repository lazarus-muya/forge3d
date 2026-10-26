// SPDX-License-Identifier: Apache-2.0
// Checks `forge3d-editor --open <model> --capture` (ctest fixture
// integration.editor.open_gltf): the framed model covers a good part of the
// image centre and is shaded (Lambert from the sun gives a range of tones).
#include <doctest/doctest.h>

#include "f3d/core/image.hpp"
#include "f3d/editor/frame_constants.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

TEST_CASE("editor mesh capture: model is visible and lit") {
    const char* path = std::getenv("F3D_MESH_PNG");
    if (!path) {
        MESSAGE("F3D_MESH_PNG not set; run through ctest integration.editor.open_gltf.check");
        return;
    }
    const auto image = f3d::core::read_png(path);
    REQUIRE(image.has_value());

    int background[3];
    for (int c = 0; c < 3; ++c) {
        background[c] =
            static_cast<int>(std::lround(f3d::core::linear_to_srgb(f3d::editor::kViewportClearColor[c]) * 255.0f));
    }
    // Central 50% of the image, where the framed model sits.
    std::size_t total = 0, model = 0;
    int min_luma = 255, max_luma = 0;
    for (std::uint32_t y = image->height / 4; y < image->height * 3 / 4; ++y) {
        for (std::uint32_t x = image->width / 4; x < image->width * 3 / 4; ++x) {
            const auto p = image->pixel(x, y);
            ++total;
            const int luma = (p[0] * 2 + p[1] * 5 + p[2]) / 8;
            const bool is_background = std::abs(p[0] - background[0]) <= 12 && std::abs(p[1] - background[1]) <= 12 &&
                                       std::abs(p[2] - background[2]) <= 12;
            // Grid lines are dark grey; the lit model is much brighter.
            if (!is_background && luma > 90) {
                ++model;
                min_luma = std::min(min_luma, luma);
                max_luma = std::max(max_luma, luma);
            }
        }
    }
    INFO("model coverage ", static_cast<double>(model) / static_cast<double>(total), ", luma ", min_luma, "..",
         max_luma);
    CHECK(static_cast<double>(model) / static_cast<double>(total) > 0.05);
    CHECK(max_luma - min_luma > 40); // shading, not a flat silhouette
}
