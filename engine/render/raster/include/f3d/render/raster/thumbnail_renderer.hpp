// SPDX-License-Identifier: Apache-2.0
#pragma once

// Offscreen model thumbnails (phases.md P3.3): a model rendered alone, framed
// from a three-quarter view, read back as an image for the asset browser.

#include "f3d/assets/mesh.hpp"
#include "f3d/core/image.hpp"
#include "f3d/render/raster/raster_engine.hpp"

#include <cstdint>

namespace f3d::render::raster {

class ThumbnailRenderer {
public:
    ThumbnailRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders);

    /// Renders `model` (its node hierarchy and meshes) into a `size` x `size`
    /// sRGB-encoded RGBA8 image. Synchronous (waits for the GPU).
    [[nodiscard]] core::Image render(const assets::ModelScene& model, std::uint32_t size);

private:
    void ensure_targets(std::uint32_t size);

    rhi::Device& device_;
    RasterEngine renderer_;
    rhi::Texture output_;
    std::uint32_t size_ = 0;
};

} // namespace f3d::render::raster
