// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/image_compare.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace f3d::core {
namespace {

constexpr int kRadius = 5; // 11x11 window
constexpr double kSigma = 1.5;
constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);

std::vector<double> luma(const Image& image) {
    std::vector<double> out(static_cast<std::size_t>(image.width) * image.height);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint8_t* p = image.rgba.data() + i * 4;
        out[i] = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    }
    return out;
}

/// Separable Gaussian blur with clamped edges.
std::vector<double> blur(const std::vector<double>& in, std::uint32_t w, std::uint32_t h) {
    std::array<double, 2 * kRadius + 1> kernel{};
    double sum = 0.0;
    for (int i = -kRadius; i <= kRadius; ++i) {
        kernel[static_cast<std::size_t>(i + kRadius)] = std::exp(-(i * i) / (2.0 * kSigma * kSigma));
        sum += kernel[static_cast<std::size_t>(i + kRadius)];
    }
    for (double& k : kernel) {
        k /= sum;
    }
    const auto iw = static_cast<int>(w);
    const auto ih = static_cast<int>(h);
    std::vector<double> tmp(in.size());
    std::vector<double> out(in.size());
    for (int y = 0; y < ih; ++y) {
        for (int x = 0; x < iw; ++x) {
            double v = 0.0;
            for (int i = -kRadius; i <= kRadius; ++i) {
                const int sx = std::clamp(x + i, 0, iw - 1);
                v += kernel[static_cast<std::size_t>(i + kRadius)] * in[static_cast<std::size_t>(y * iw + sx)];
            }
            tmp[static_cast<std::size_t>(y * iw + x)] = v;
        }
    }
    for (int y = 0; y < ih; ++y) {
        for (int x = 0; x < iw; ++x) {
            double v = 0.0;
            for (int i = -kRadius; i <= kRadius; ++i) {
                const int sy = std::clamp(y + i, 0, ih - 1);
                v += kernel[static_cast<std::size_t>(i + kRadius)] * tmp[static_cast<std::size_t>(sy * iw + x)];
            }
            out[static_cast<std::size_t>(y * iw + x)] = v;
        }
    }
    return out;
}

} // namespace

ImageComparison compare_images(const Image& a, const Image& b) {
    ImageComparison out;
    out.same_size = a.width == b.width && a.height == b.height && !a.empty() &&
                    a.rgba.size() == static_cast<std::size_t>(a.width) * a.height * 4 && a.rgba.size() == b.rgba.size();
    if (!out.same_size) {
        return out;
    }
    const std::size_t pixels = static_cast<std::size_t>(a.width) * a.height;
    double total = 0.0;
    for (std::size_t i = 0; i < pixels; ++i) {
        bool differs = false;
        for (std::size_t c = 0; c < 3; ++c) {
            const auto d = static_cast<std::uint32_t>(std::abs(int(a.rgba[i * 4 + c]) - int(b.rgba[i * 4 + c])));
            total += d;
            out.max_difference = std::max(out.max_difference, d);
            differs = differs || d > 8;
        }
        out.differing_pixels += differs ? 1 : 0;
    }
    out.mean_difference = total / static_cast<double>(pixels * 3);

    const std::vector<double> x = luma(a);
    const std::vector<double> y = luma(b);
    std::vector<double> xx(pixels), yy(pixels), xy(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        xx[i] = x[i] * x[i];
        yy[i] = y[i] * y[i];
        xy[i] = x[i] * y[i];
    }
    const auto mx = blur(x, a.width, a.height);
    const auto my = blur(y, a.width, a.height);
    const auto sxx = blur(xx, a.width, a.height);
    const auto syy = blur(yy, a.width, a.height);
    const auto sxy = blur(xy, a.width, a.height);
    std::vector<double> map(pixels);
    double sum = 0.0;
    for (std::size_t i = 0; i < pixels; ++i) {
        const double vx = std::max(sxx[i] - mx[i] * mx[i], 0.0);
        const double vy = std::max(syy[i] - my[i] * my[i], 0.0);
        const double cov = sxy[i] - mx[i] * my[i];
        map[i] = ((2.0 * mx[i] * my[i] + kC1) * (2.0 * cov + kC2)) /
                 ((mx[i] * mx[i] + my[i] * my[i] + kC1) * (vx + vy + kC2));
        sum += map[i];
    }
    out.ssim = sum / static_cast<double>(pixels);

    // The worst 16x16 block.
    out.min_ssim = 1.0;
    for (std::uint32_t by = 0; by < a.height; by += 16) {
        for (std::uint32_t bx = 0; bx < a.width; bx += 16) {
            double block = 0.0;
            std::size_t count = 0;
            for (std::uint32_t py = by; py < std::min(by + 16, a.height); ++py) {
                for (std::uint32_t px = bx; px < std::min(bx + 16, a.width); ++px) {
                    block += map[static_cast<std::size_t>(py) * a.width + px];
                    ++count;
                }
            }
            out.min_ssim = std::min(out.min_ssim, block / static_cast<double>(count));
        }
    }
    return out;
}

Image difference_image(const Image& a, const Image& b) {
    Image out;
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        return out;
    }
    out.width = a.width;
    out.height = a.height;
    out.rgba.resize(a.rgba.size());
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const int d = std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])) * 4;
            out.rgba[i + c] = static_cast<std::uint8_t>(std::min(d, 255));
        }
        out.rgba[i + 3] = 255;
    }
    return out;
}

} // namespace f3d::core
