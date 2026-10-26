// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/image_compare.hpp"

#include <algorithm>
#include <random>

using namespace f3d::core;

namespace {

/// A smooth gradient with a few shapes, like a render.
Image test_image(std::uint32_t w = 96, std::uint32_t h = 64) {
    Image image;
    image.width = w;
    image.height = h;
    image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = image.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            const bool square = x > 20 && x < 40 && y > 20 && y < 40;
            p[0] = static_cast<std::uint8_t>(square ? 220 : x * 2);
            p[1] = static_cast<std::uint8_t>(square ? 40 : y * 3);
            p[2] = static_cast<std::uint8_t>(128);
            p[3] = 255;
        }
    }
    return image;
}

} // namespace

TEST_CASE("image compare: identical images") {
    const Image a = test_image();
    const ImageComparison r = compare_images(a, a);
    CHECK(r.same_size);
    CHECK(r.ssim == doctest::Approx(1.0));
    CHECK(r.min_ssim == doctest::Approx(1.0));
    CHECK(r.max_difference == 0u);
    CHECK(r.differing_pixels == 0u);
}

TEST_CASE("image compare: noise lowers SSIM a little, a missing shape a lot") {
    const Image a = test_image();
    Image noisy = a;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> noise(-3, 3);
    for (std::size_t i = 0; i < noisy.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            noisy.rgba[i + c] = static_cast<std::uint8_t>(std::clamp(int(noisy.rgba[i + c]) + noise(rng), 0, 255));
        }
    }
    const ImageComparison slight = compare_images(a, noisy);
    CHECK(slight.ssim > 0.95);
    CHECK(slight.ssim < 1.0);
    CHECK(slight.max_difference <= 3u);
    CHECK(slight.differing_pixels == 0u); // within the 8-level tolerance

    Image missing = a;
    for (std::uint32_t y = 21; y < 40; ++y) {
        for (std::uint32_t x = 21; x < 40; ++x) {
            std::uint8_t* p = missing.rgba.data() + (static_cast<std::size_t>(y) * a.width + x) * 4;
            p[0] = static_cast<std::uint8_t>(x * 2);
            p[1] = static_cast<std::uint8_t>(y * 3);
        }
    }
    const ImageComparison changed = compare_images(a, missing);
    // The golden tests' default limit for the worst block is 0.90: noise passes it, a
    // missing shape doesn't.
    CHECK(slight.min_ssim > 0.9);
    CHECK(changed.min_ssim < 0.9);
    CHECK(changed.differing_pixels > 300u);

    const Image diff = difference_image(a, missing);
    REQUIRE(diff.width == a.width);
    CHECK(diff.pixel(30, 30)[0] == 255); // amplified, clamped
    CHECK(diff.pixel(5, 5) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
}

TEST_CASE("image compare: different sizes don't compare") {
    CHECK_FALSE(compare_images(test_image(96, 64), test_image(64, 96)).same_size);
    CHECK(difference_image(test_image(96, 64), test_image(64, 96)).empty());
}
