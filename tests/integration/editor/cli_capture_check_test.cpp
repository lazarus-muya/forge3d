// SPDX-License-Identifier: Apache-2.0
// Checks the PNGs `forge3d-cli ... run-op viewport.capture` wrote headlessly
// (ctest fixture integration.cli.capture, phases.md P4.4): the noon sky scene in
// every view mode, from its "View" bookmark.
#include <doctest/doctest.h>

#include "f3d/core/image.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace {

struct Rgb {
    int r, g, b;
};

Rgb at(const f3d::core::Image& image, float fx, float fy) {
    const auto p = image.pixel(static_cast<std::uint32_t>(fx * static_cast<float>(image.width - 1)),
                               static_cast<std::uint32_t>(fy * static_cast<float>(image.height - 1)));
    return {p[0], p[1], p[2]};
}

double mean_difference(const f3d::core::Image& a, const f3d::core::Image& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        sum += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    }
    return sum / static_cast<double>(a.rgba.size());
}

} // namespace

TEST_CASE("cli capture: viewport.capture writes every view mode headlessly") {
    const char* dir = std::getenv("F3D_CLI_CAPTURE_DIR");
    if (!dir) {
        MESSAGE("F3D_CLI_CAPTURE_DIR not set; run through ctest integration.cli.capture.check");
        return;
    }
    std::map<std::string, f3d::core::Image> images;
    for (const char* mode : {"lit", "unlit", "wireframe", "normals", "cascades"}) {
        const auto image = f3d::core::read_png(std::filesystem::path(dir) / (std::string("sky_") + mode + ".png"));
        REQUIRE_MESSAGE(image.has_value(), mode);
        CHECK(image->width == 640);
        CHECK(image->height == 360);
        images[mode] = *image;
    }

    // Lit: a blue sky above, the red ground below.
    const Rgb sky = at(images["lit"], 0.5f, 0.05f);
    CHECK(sky.b > sky.r + 20);
    const Rgb ground = at(images["lit"], 0.5f, 0.95f);
    CHECK(ground.r > ground.g + 40);

    // Normals: the ground faces +Y, shown raw as (0.5, 1, 0.5).
    const Rgb up = at(images["normals"], 0.5f, 0.95f);
    CHECK(std::abs(up.r - 128) <= 3);
    CHECK(up.g >= 250);
    CHECK(std::abs(up.b - 128) <= 3);

    // Unlit: the material's base colour, the same in light and shadow; no sky.
    const Rgb unlit_ground = at(images["unlit"], 0.5f, 0.95f);
    CHECK(unlit_ground.r > 150);
    CHECK(unlit_ground.g < 60);
    const Rgb unlit_sky = at(images["unlit"], 0.5f, 0.05f);
    CHECK(std::abs(unlit_sky.r - unlit_sky.b) < 20); // the grey background, not a blue sky

    // Wireframe: light edges over the background.
    std::size_t bright = 0;
    for (std::size_t i = 0; i < images["wireframe"].rgba.size(); i += 4) {
        bright += images["wireframe"].rgba[i + 1] > 200 ? 1 : 0;
    }
    CHECK(bright > 100);

    // Every mode looks different; cascades tint the lit image.
    CHECK(mean_difference(images["lit"], images["cascades"]) > 3.0);
    CHECK(mean_difference(images["lit"], images["unlit"]) > 3.0);
    CHECK(mean_difference(images["normals"], images["wireframe"]) > 3.0);
}
