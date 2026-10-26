// SPDX-License-Identifier: Apache-2.0
#pragma once

// Image comparison for golden-image tests (phases.md P4.5): SSIM on luma (Wang et
// al. 2004: 11x11 Gaussian window, sigma 1.5, K1 0.01, K2 0.03) plus plain
// difference statistics, and a diff image for failure reports.

#include "f3d/core/image.hpp"

#include <cstddef>
#include <cstdint>

namespace f3d::core {

struct ImageComparison {
    bool same_size = false;
    double ssim = 0.0;                ///< mean SSIM, 1 = identical structure
    double min_ssim = 0.0;            ///< the worst 16x16 block's mean SSIM (catches small local errors)
    double mean_difference = 0.0;     ///< mean absolute channel difference (0..255)
    std::uint32_t max_difference = 0; ///< largest channel difference (0..255)
    std::size_t differing_pixels = 0; ///< pixels with any channel differing by more than 8
};

/// Compares RGB (alpha ignored). Images of different sizes compare as not the same size.
[[nodiscard]] ImageComparison compare_images(const Image& a, const Image& b);

/// |a - b| per channel, amplified 4x, opaque; for looking at what changed.
[[nodiscard]] Image difference_image(const Image& a, const Image& b);

} // namespace f3d::core
