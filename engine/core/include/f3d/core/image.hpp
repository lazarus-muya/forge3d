// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace f3d::core {

/// 8-bit RGBA image in row-major order, top row first. Colour values are
/// stored as given (typically sRGB-encoded).
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; ///< width * height * 4 bytes

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] std::array<std::uint8_t, 4> pixel(std::uint32_t x, std::uint32_t y) const noexcept;
};

/// Floating-point RGBA image (linear values), row-major, top row first.
struct ImageF {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba; ///< width * height * 4 floats

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

/// Decodes PNG, JPEG, TGA, BMP, GIF (first frame), PSD or PIC data to RGBA8.
[[nodiscard]] std::optional<Image> decode_image(std::span<const std::uint8_t> encoded);
/// Decodes Radiance .hdr data to linear RGBA32F.
[[nodiscard]] std::optional<ImageF> decode_image_hdr(std::span<const std::uint8_t> encoded);
/// Encodes RGBA8 as PNG in memory (empty on failure).
[[nodiscard]] std::vector<std::uint8_t> encode_png(const Image& image);

/// Writes an RGBA PNG. Returns false on I/O failure or an empty image.
bool write_png(const std::filesystem::path& path, const Image& image);

/// Reads any PNG (converted to RGBA8). Returns nullopt on failure.
[[nodiscard]] std::optional<Image> read_png(const std::filesystem::path& path);

/// sRGB transfer function, linear [0,1] -> encoded [0,1].
[[nodiscard]] float linear_to_srgb(float linear) noexcept;
/// Inverse sRGB transfer function, encoded [0,1] -> linear [0,1].
[[nodiscard]] float srgb_to_linear(float encoded) noexcept;

} // namespace f3d::core
