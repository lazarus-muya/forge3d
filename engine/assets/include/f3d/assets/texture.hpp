// SPDX-License-Identifier: Apache-2.0
#pragma once

// Texture import and cooking (phases.md P3.2): source images (stb_image,
// tinyexr) -> mip chain -> block compression (bc7enc_rdo: BC1/BC3/BC4/BC5/BC7)
// -> cooked .f3dtex.

#include "f3d/core/image.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::assets {

/// GPU formats of cooked textures. Values are stored in .f3dtex files: don't renumber.
enum class TextureFormat : std::uint32_t {
    RGBA8 = 0,   ///< uncompressed (sRGB or linear)
    BC1 = 1,     ///< RGB, 4 bpp
    BC3 = 2,     ///< RGBA, 8 bpp
    BC4 = 3,     ///< R, 4 bpp
    BC5 = 4,     ///< RG, 8 bpp (normal maps: Z reconstructed)
    BC7 = 5,     ///< RGBA, 8 bpp, high quality
    RGBA16F = 6, ///< HDR, uncompressed half floats
};

[[nodiscard]] std::string_view to_string(TextureFormat format) noexcept;
/// Bytes per 4x4 block for block formats, 0 for uncompressed ones.
[[nodiscard]] std::size_t block_bytes(TextureFormat format) noexcept;
/// Bytes of one mip level.
[[nodiscard]] std::size_t mip_bytes(TextureFormat format, std::uint32_t width, std::uint32_t height) noexcept;

/// How a texture's values are used, which decides colour space and compression.
enum class TextureKind : std::uint8_t {
    Color,     ///< sRGB colour (base colour, emissive): BC7 sRGB
    Normal,    ///< tangent-space normal map: BC5, mips renormalised
    Linear,    ///< packed linear data (ORM): BC7 linear
    Grayscale, ///< one channel (roughness, AO, height): BC4
    Hdr,       ///< high dynamic range (.exr, .hdr): RGBA16F
};

[[nodiscard]] std::string_view to_string(TextureKind kind) noexcept;

/// Import settings, stored in the texture's .meta "settings".
struct TextureSettings {
    TextureKind kind = TextureKind::Color;
    /// "auto" (by kind), "none" (RGBA8 / RGBA16F), "bc1", "bc3", "bc4", "bc5", "bc7".
    std::string compression = "auto";
    bool mipmaps = true;

    /// Defaults from the file name: "*_n" / "*normal*" -> Normal, "*_orm*" -> Linear,
    /// roughness/metallic/AO/height/mask -> Grayscale, .exr/.hdr -> Hdr, else Color.
    [[nodiscard]] static TextureSettings defaults_for(std::string_view path);
    /// Reads the fields present in `json` over `defaults`.
    [[nodiscard]] static TextureSettings from_json(const nlohmann::json& json, const TextureSettings& defaults);
    [[nodiscard]] nlohmann::json to_json() const;
};

/// A decoded source image: 8-bit (as stored) or HDR (linear floats).
struct SourceImage {
    core::Image ldr;
    core::ImageF hdr;
    [[nodiscard]] bool is_hdr() const noexcept { return !hdr.empty(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return is_hdr() ? hdr.width : ldr.width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return is_hdr() ? hdr.height : ldr.height; }
};

/// Decodes a source image; `extension` (".png", ".exr", ...) picks the decoder.
[[nodiscard]] std::optional<SourceImage>
decode_texture_source(std::span<const std::uint8_t> data, std::string_view extension, std::string* error = nullptr);

struct TextureMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> data;
};

struct CookedTexture {
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = false;
    std::vector<TextureMip> mips; ///< largest first

    [[nodiscard]] std::uint32_t width() const noexcept { return mips.empty() ? 0 : mips[0].width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return mips.empty() ? 0 : mips[0].height; }
};

/// Builds the mip chain and compresses it. Thread-safe (the encoders' tables are
/// initialised once).
[[nodiscard]] CookedTexture cook_texture(const SourceImage& source, const TextureSettings& settings);

/// The .f3dtex file format (docs/architecture/assets.md).
[[nodiscard]] std::vector<std::uint8_t> write_f3dtex(const CookedTexture& texture);
[[nodiscard]] std::optional<CookedTexture> read_f3dtex(std::span<const std::uint8_t> data,
                                                       std::string* error = nullptr);

/// Decodes one mip of a cooked texture to RGBA8 (BC formats unpacked; HDR
/// tonemapped by clamping). For previews, thumbnails and tests.
[[nodiscard]] core::Image decode_mip(const CookedTexture& texture, std::size_t mip = 0);

/// A thumbnail: the largest mip no bigger than `max_size` on either side (or the
/// smallest mip), decoded to RGBA8.
[[nodiscard]] core::Image texture_thumbnail(const CookedTexture& texture, std::uint32_t max_size);

} // namespace f3d::assets
