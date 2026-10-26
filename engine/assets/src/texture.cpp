// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/texture.hpp"

#include "f3d/core/math.hpp"
#include "f3d/core/profile.hpp"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <glm/gtc/packing.hpp>
#include <nlohmann/json.hpp>
#include <rgbcx.h>
#include <tinyexr.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace f3d::assets {
namespace {

constexpr std::array<char, 4> kMagic{'F', '3', 'D', 'T'};
constexpr std::uint32_t kVersion = 1;

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void init_encoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
    });
}

/// One mip level as linear floats (RGBA), the working format of the mip builder.
struct FloatImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;
    float* at(std::uint32_t x, std::uint32_t y) { return &rgba[(static_cast<std::size_t>(y) * width + x) * 4]; }
    const float* at(std::uint32_t x, std::uint32_t y) const {
        return &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
    }
};

/// Source -> working floats: colour is linearised (sRGB), normals mapped to [-1, 1].
FloatImage to_float(const SourceImage& source, TextureKind kind) {
    FloatImage out;
    out.width = source.width();
    out.height = source.height();
    if (source.is_hdr()) {
        out.rgba = source.hdr.rgba;
        return out;
    }
    out.rgba.resize(source.ldr.rgba.size());
    for (std::size_t i = 0; i < source.ldr.rgba.size(); ++i) {
        const float v = source.ldr.rgba[i] / 255.0f;
        const bool alpha = i % 4 == 3;
        if (kind == TextureKind::Color && !alpha) {
            out.rgba[i] = core::srgb_to_linear(v);
        } else if (kind == TextureKind::Normal && !alpha) {
            out.rgba[i] = v * 2.0f - 1.0f;
        } else {
            out.rgba[i] = v;
        }
    }
    return out;
}

/// The next mip: 2x2 box filter (odd edges clamp), normals renormalised.
FloatImage downsample(const FloatImage& src, TextureKind kind) {
    FloatImage dst;
    dst.width = std::max(1u, src.width / 2);
    dst.height = std::max(1u, src.height / 2);
    dst.rgba.resize(static_cast<std::size_t>(dst.width) * dst.height * 4);
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            const std::uint32_t x0 = std::min(x * 2, src.width - 1);
            const std::uint32_t x1 = std::min(x * 2 + 1, src.width - 1);
            const std::uint32_t y0 = std::min(y * 2, src.height - 1);
            const std::uint32_t y1 = std::min(y * 2 + 1, src.height - 1);
            float* out = dst.at(x, y);
            for (int c = 0; c < 4; ++c) {
                out[c] = 0.25f * (src.at(x0, y0)[c] + src.at(x1, y0)[c] + src.at(x0, y1)[c] + src.at(x1, y1)[c]);
            }
            if (kind == TextureKind::Normal) {
                glm::vec3 n(out[0], out[1], out[2]);
                const float len = glm::length(n);
                n = len > 1e-6f ? n / len : glm::vec3(0.0f, 0.0f, 1.0f);
                out[0] = n.x;
                out[1] = n.y;
                out[2] = n.z;
            }
        }
    }
    return dst;
}

std::uint8_t to_unorm8(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

/// Working floats -> RGBA8 as stored (sRGB re-encoded, normals packed).
std::vector<std::uint8_t> to_rgba8(const FloatImage& image, TextureKind kind) {
    std::vector<std::uint8_t> out(image.rgba.size());
    for (std::size_t i = 0; i < image.rgba.size(); ++i) {
        const bool alpha = i % 4 == 3;
        float v = image.rgba[i];
        if (kind == TextureKind::Color && !alpha) {
            v = core::linear_to_srgb(v);
        } else if (kind == TextureKind::Normal && !alpha) {
            v = v * 0.5f + 0.5f;
        }
        out[i] = to_unorm8(v);
    }
    return out;
}

TextureFormat choose_format(const TextureSettings& settings, bool hdr) {
    const std::string c = lower(settings.compression);
    if (hdr || settings.kind == TextureKind::Hdr) {
        return TextureFormat::RGBA16F; // no BC6H encoder yet
    }
    if (c == "none") {
        return TextureFormat::RGBA8;
    }
    if (c == "bc1") {
        return TextureFormat::BC1;
    }
    if (c == "bc3") {
        return TextureFormat::BC3;
    }
    if (c == "bc4") {
        return TextureFormat::BC4;
    }
    if (c == "bc5") {
        return TextureFormat::BC5;
    }
    if (c == "bc7") {
        return TextureFormat::BC7;
    }
    switch (settings.kind) {
    case TextureKind::Normal:
        return TextureFormat::BC5;
    case TextureKind::Grayscale:
        return TextureFormat::BC4;
    default:
        return TextureFormat::BC7;
    }
}

/// Compresses an RGBA8 level into blocks (edges padded by clamping).
std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height,
                                   TextureFormat format, bool perceptual) {
    const std::size_t block = block_bytes(format);
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bw) * bh * block);
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (!perceptual) {
        bc7enc_compress_block_params_init_linear_weights(&params);
    }
    std::array<std::uint8_t, 64> pixels{};
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const std::uint32_t sx = std::min(bx * 4 + x, width - 1);
                    const std::uint32_t sy = std::min(by * 4 + y, height - 1);
                    std::memcpy(&pixels[(y * 4 + x) * 4], &rgba[(static_cast<std::size_t>(sy) * width + sx) * 4], 4);
                }
            }
            std::uint8_t* dst = &out[(static_cast<std::size_t>(by) * bw + bx) * block];
            switch (format) {
            case TextureFormat::BC1:
                rgbcx::encode_bc1(10, dst, pixels.data(), false, false);
                break;
            case TextureFormat::BC3:
                rgbcx::encode_bc3(10, dst, pixels.data());
                break;
            case TextureFormat::BC4:
                rgbcx::encode_bc4(dst, pixels.data(), 4);
                break;
            case TextureFormat::BC5:
                rgbcx::encode_bc5(dst, pixels.data(), 0, 1, 4);
                break;
            case TextureFormat::BC7:
                bc7enc_compress_block(dst, pixels.data(), &params);
                break;
            default:
                break;
            }
        }
    }
    return out;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

std::string_view to_string(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::RGBA8:
        return "RGBA8";
    case TextureFormat::BC1:
        return "BC1";
    case TextureFormat::BC3:
        return "BC3";
    case TextureFormat::BC4:
        return "BC4";
    case TextureFormat::BC5:
        return "BC5";
    case TextureFormat::BC7:
        return "BC7";
    case TextureFormat::RGBA16F:
        return "RGBA16F";
    }
    return "?";
}

std::size_t block_bytes(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::BC1:
    case TextureFormat::BC4:
        return 8;
    case TextureFormat::BC3:
    case TextureFormat::BC5:
    case TextureFormat::BC7:
        return 16;
    default:
        return 0;
    }
}

std::size_t mip_bytes(TextureFormat format, std::uint32_t width, std::uint32_t height) noexcept {
    if (const std::size_t block = block_bytes(format)) {
        return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * block;
    }
    const std::size_t texel = format == TextureFormat::RGBA16F ? 8 : 4;
    return static_cast<std::size_t>(width) * height * texel;
}

std::string_view to_string(TextureKind kind) noexcept {
    switch (kind) {
    case TextureKind::Color:
        return "color";
    case TextureKind::Normal:
        return "normal";
    case TextureKind::Linear:
        return "linear";
    case TextureKind::Grayscale:
        return "grayscale";
    case TextureKind::Hdr:
        return "hdr";
    }
    return "?";
}

TextureSettings TextureSettings::defaults_for(std::string_view path) {
    TextureSettings s;
    const std::string p = lower(path);
    const auto slash = p.find_last_of('/');
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    const std::string stem = name.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
    const auto has = [&](const char* token) { return stem.find(token) != std::string::npos; };
    if (ext == ".exr" || ext == ".hdr") {
        s.kind = TextureKind::Hdr;
    } else if (has("normal") || stem.ends_with("_n") || stem.ends_with("_nrm") || stem.ends_with("_nor")) {
        s.kind = TextureKind::Normal;
    } else if (has("_orm") || has("_arm") || has("metallicroughness") || has("occlusionroughnessmetallic")) {
        s.kind = TextureKind::Linear;
    } else if (has("rough") || has("metal") || stem.ends_with("_ao") || has("occlusion") || has("height") ||
               has("_disp") || has("mask")) {
        s.kind = TextureKind::Grayscale;
    }
    return s;
}

TextureSettings TextureSettings::from_json(const nlohmann::json& json, const TextureSettings& defaults) {
    TextureSettings s = defaults;
    if (!json.is_object()) {
        return s;
    }
    const std::string kind = json.value("kind", std::string(to_string(defaults.kind)));
    for (const TextureKind k :
         {TextureKind::Color, TextureKind::Normal, TextureKind::Linear, TextureKind::Grayscale, TextureKind::Hdr}) {
        if (kind == to_string(k)) {
            s.kind = k;
        }
    }
    s.compression = json.value("compression", defaults.compression);
    s.mipmaps = json.value("mipmaps", defaults.mipmaps);
    return s;
}

nlohmann::json TextureSettings::to_json() const {
    return {{"kind", std::string(to_string(kind))}, {"compression", compression}, {"mipmaps", mipmaps}};
}

std::optional<SourceImage> decode_texture_source(std::span<const std::uint8_t> data, std::string_view extension,
                                                 std::string* error) {
    const std::string ext = lower(extension);
    SourceImage image;
    if (ext == ".exr") {
        float* rgba = nullptr;
        int w = 0, h = 0;
        const char* err = nullptr;
        if (LoadEXRFromMemory(&rgba, &w, &h, data.data(), data.size(), &err) != TINYEXR_SUCCESS) {
            if (error) {
                *error = err ? err : "cannot decode EXR";
            }
            FreeEXRErrorMessage(err);
            return std::nullopt;
        }
        image.hdr.width = static_cast<std::uint32_t>(w);
        image.hdr.height = static_cast<std::uint32_t>(h);
        image.hdr.rgba.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
        std::free(rgba);
        return image;
    }
    if (ext == ".hdr") {
        if (auto hdr = core::decode_image_hdr(data)) {
            image.hdr = std::move(*hdr);
            return image;
        }
    } else if (auto ldr = core::decode_image(data)) {
        image.ldr = std::move(*ldr);
        return image;
    }
    if (error) {
        *error = "cannot decode " + std::string(extension) + " image";
    }
    return std::nullopt;
}

CookedTexture cook_texture(const SourceImage& source, const TextureSettings& settings) {
    F3D_ZONE;
    init_encoders();
    CookedTexture out;
    const bool hdr = source.is_hdr();
    out.format = choose_format(settings, hdr);
    out.srgb = settings.kind == TextureKind::Color && !hdr && out.format != TextureFormat::BC4 &&
               out.format != TextureFormat::BC5;
    // The kind used for filtering: HDR data is always linear.
    const TextureKind filter_kind = hdr ? TextureKind::Hdr : settings.kind;

    FloatImage level = to_float(source, filter_kind);
    while (true) {
        TextureMip mip;
        mip.width = level.width;
        mip.height = level.height;
        if (out.format == TextureFormat::RGBA16F) {
            mip.data.resize(level.rgba.size() * 2);
            for (std::size_t i = 0; i < level.rgba.size(); ++i) {
                const std::uint16_t half = glm::packHalf1x16(level.rgba[i]);
                std::memcpy(&mip.data[i * 2], &half, 2);
            }
        } else {
            std::vector<std::uint8_t> rgba = to_rgba8(level, filter_kind);
            if (out.format == TextureFormat::RGBA8) {
                mip.data = std::move(rgba);
            } else {
                mip.data = compress(rgba, level.width, level.height, out.format, out.srgb);
            }
        }
        out.mips.push_back(std::move(mip));
        if (!settings.mipmaps || (level.width == 1 && level.height == 1)) {
            break;
        }
        level = downsample(level, filter_kind);
    }
    return out;
}

std::vector<std::uint8_t> write_f3dtex(const CookedTexture& texture) {
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    put32(out, kVersion);
    put32(out, static_cast<std::uint32_t>(texture.format));
    put32(out, texture.srgb ? 1u : 0u);
    put32(out, static_cast<std::uint32_t>(texture.mips.size()));
    for (const TextureMip& mip : texture.mips) {
        put32(out, mip.width);
        put32(out, mip.height);
        put32(out, static_cast<std::uint32_t>(mip.data.size()));
    }
    for (const TextureMip& mip : texture.mips) {
        out.insert(out.end(), mip.data.begin(), mip.data.end());
    }
    return out;
}

std::optional<CookedTexture> read_f3dtex(std::span<const std::uint8_t> data, std::string* error) {
    const auto fail = [&](const char* why) -> std::optional<CookedTexture> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (data.size() < 20 || !std::equal(kMagic.begin(), kMagic.end(), data.begin())) {
        return fail("not an .f3dtex file");
    }
    if (get32(&data[4]) != kVersion) {
        return fail("unsupported .f3dtex version");
    }
    CookedTexture texture;
    const std::uint32_t format = get32(&data[8]);
    if (format > static_cast<std::uint32_t>(TextureFormat::RGBA16F)) {
        return fail("unknown texture format");
    }
    texture.format = static_cast<TextureFormat>(format);
    texture.srgb = get32(&data[12]) != 0;
    const std::uint32_t count = get32(&data[16]);
    std::size_t pos = 20;
    if (count == 0 || count > 32 || data.size() < pos + count * 12ull) {
        return fail("corrupt mip table");
    }
    texture.mips.resize(count);
    for (TextureMip& mip : texture.mips) {
        mip.width = get32(&data[pos]);
        mip.height = get32(&data[pos + 4]);
        const std::uint32_t size = get32(&data[pos + 8]);
        if (mip.width == 0 || mip.height == 0 || size != mip_bytes(texture.format, mip.width, mip.height)) {
            return fail("corrupt mip table");
        }
        mip.data.resize(size);
        pos += 12;
    }
    for (TextureMip& mip : texture.mips) {
        if (data.size() < pos + mip.data.size()) {
            return fail("truncated mip data");
        }
        std::memcpy(mip.data.data(), &data[pos], mip.data.size());
        pos += mip.data.size();
    }
    return texture;
}

core::Image texture_thumbnail(const CookedTexture& texture, std::uint32_t max_size) {
    std::size_t best = texture.mips.empty() ? 0 : texture.mips.size() - 1;
    for (std::size_t i = 0; i < texture.mips.size(); ++i) {
        if (texture.mips[i].width <= max_size && texture.mips[i].height <= max_size) {
            best = i;
            break;
        }
    }
    return decode_mip(texture, best);
}

core::Image decode_mip(const CookedTexture& texture, std::size_t index) {
    init_encoders(); // the BC1/BC3 unpackers use rgbcx's tables too
    core::Image image;
    if (index >= texture.mips.size()) {
        return image;
    }
    const TextureMip& mip = texture.mips[index];
    image.width = mip.width;
    image.height = mip.height;
    image.rgba.assign(static_cast<std::size_t>(mip.width) * mip.height * 4, 255);
    if (texture.format == TextureFormat::RGBA8) {
        image.rgba = mip.data;
        return image;
    }
    if (texture.format == TextureFormat::RGBA16F) {
        for (std::size_t i = 0; i < image.rgba.size(); ++i) {
            std::uint16_t half = 0;
            std::memcpy(&half, &mip.data[i * 2], 2);
            float v = glm::unpackHalf1x16(half);
            if (i % 4 != 3) {
                v = core::linear_to_srgb(v); // display: clamp + sRGB
            }
            image.rgba[i] = to_unorm8(v);
        }
        return image;
    }
    const std::size_t block = block_bytes(texture.format);
    const std::uint32_t bw = (mip.width + 3) / 4;
    const std::uint32_t bh = (mip.height + 3) / 4;
    std::array<std::uint8_t, 64> pixels{};
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::uint8_t* src = &mip.data[(static_cast<std::size_t>(by) * bw + bx) * block];
            pixels.fill(255);
            switch (texture.format) {
            case TextureFormat::BC1:
                rgbcx::unpack_bc1(src, pixels.data());
                break;
            case TextureFormat::BC3:
                rgbcx::unpack_bc3(src, pixels.data());
                break;
            case TextureFormat::BC4:
                rgbcx::unpack_bc4(src, pixels.data(), 4);
                for (int p = 0; p < 16; ++p) {
                    pixels[p * 4 + 1] = pixels[p * 4 + 2] = pixels[p * 4]; // grey
                }
                break;
            case TextureFormat::BC5:
                rgbcx::unpack_bc5(src, pixels.data(), 0, 1, 4);
                for (int p = 0; p < 16; ++p) {
                    // Reconstruct Z for display, as the shader will.
                    const float x = pixels[p * 4] / 127.5f - 1.0f;
                    const float y = pixels[p * 4 + 1] / 127.5f - 1.0f;
                    pixels[p * 4 + 2] = to_unorm8(std::sqrt(std::max(0.0f, 1.0f - x * x - y * y)) * 0.5f + 0.5f);
                }
                break;
            case TextureFormat::BC7:
                bc7decomp::unpack_bc7(src, reinterpret_cast<bc7decomp::color_rgba*>(pixels.data()));
                break;
            default:
                break;
            }
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const std::uint32_t px = bx * 4 + x;
                    const std::uint32_t py = by * 4 + y;
                    if (px < mip.width && py < mip.height) {
                        std::memcpy(&image.rgba[(static_cast<std::size_t>(py) * mip.width + px) * 4],
                                    &pixels[(y * 4 + x) * 4], 4);
                    }
                }
            }
        }
    }
    return image;
}

} // namespace f3d::assets
