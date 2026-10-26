// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/image.hpp"

#include <cmath>
#include <cstdio>
#include <memory>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
// The texture importer (P3.2) decodes these; everything else stays out of the build.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#include <stb_image.h>
#include <stb_image_write.h>

namespace f3d::core {
namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr open_file(const std::filesystem::path& path, bool write) {
#if defined(_WIN32)
    return FilePtr(_wfopen(path.c_str(), write ? L"wb" : L"rb"));
#else
    return FilePtr(std::fopen(path.c_str(), write ? "wb" : "rb"));
#endif
}

void write_to_file(void* context, void* data, int size) {
    std::fwrite(data, 1, static_cast<std::size_t>(size), static_cast<std::FILE*>(context));
}

} // namespace

std::array<std::uint8_t, 4> Image::pixel(std::uint32_t x, std::uint32_t y) const noexcept {
    if (x >= width || y >= height) {
        return {0, 0, 0, 0};
    }
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

bool write_png(const std::filesystem::path& path, const Image& image) {
    if (image.empty() || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        return false;
    }
    FilePtr file = open_file(path, true);
    if (!file) {
        return false;
    }
    const int ok =
        stbi_write_png_to_func(write_to_file, file.get(), static_cast<int>(image.width), static_cast<int>(image.height),
                               4, image.rgba.data(), static_cast<int>(image.width) * 4);
    return ok != 0 && std::ferror(file.get()) == 0;
}

std::optional<Image> read_png(const std::filesystem::path& path) {
    FilePtr file = open_file(path, false);
    if (!file) {
        return std::nullopt;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* data = stbi_load_from_file(file.get(), &w, &h, &channels, 4);
    if (!data) {
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.rgba.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    return image;
}

std::optional<Image> decode_image(std::span<const std::uint8_t> encoded) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* data = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels, 4);
    if (!data) {
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.rgba.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    return image;
}

std::optional<ImageF> decode_image_hdr(std::span<const std::uint8_t> encoded) {
    int w = 0, h = 0, channels = 0;
    float* data = stbi_loadf_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels, 4);
    if (!data) {
        return std::nullopt;
    }
    ImageF image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.rgba.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    return image;
}

std::vector<std::uint8_t> encode_png(const Image& image) {
    std::vector<std::uint8_t> out;
    if (image.empty()) {
        return out;
    }
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* bytes = static_cast<std::vector<std::uint8_t>*>(context);
            const auto* p = static_cast<const std::uint8_t*>(data);
            bytes->insert(bytes->end(), p, p + size);
        },
        &out, static_cast<int>(image.width), static_cast<int>(image.height), 4, image.rgba.data(),
        static_cast<int>(image.width) * 4);
    return out;
}

float srgb_to_linear(float encoded) noexcept {
    if (encoded <= 0.04045f) {
        return encoded / 12.92f;
    }
    return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float linear) noexcept {
    if (linear <= 0.0031308f) {
        return linear * 12.92f;
    }
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

} // namespace f3d::core
