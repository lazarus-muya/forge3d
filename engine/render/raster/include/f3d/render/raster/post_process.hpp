// SPDX-License-Identifier: Apache-2.0
#pragma once

// Post-processing (scene::PostProcess): bloom as a chain of half-sized levels
// (shaders/bloom_*.ps.hlsl), and the grading constants the resolve applies.

#include "f3d/core/math.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/components.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::render::raster {

/// GPU layout of shaders/bloom.hlsli `BloomConstants`.
struct BloomConstants {
    glm::vec4 source{0.0f}; ///< source texel size, 1 = first pass, threshold luminance
    glm::vec4 target{0.0f}; ///< target texel size, upsampling radius
};

/// The first visible PostProcess of `world`, if any.
[[nodiscard]] std::optional<scene::PostProcess> gather_post_process(scene::World& world);

/// Bloom levels for a radius (0-1) and a scene size: 2 to 7, fewer for tiny views.
[[nodiscard]] std::uint32_t bloom_levels(float radius, std::uint32_t width, std::uint32_t height) noexcept;

class BloomRenderer {
public:
    BloomRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders);

    void create_pipelines();
    /// Blurs `scene_color` (pre-exposed HDR) into result(): the sum of every level, at
    /// half the scene's size. False if the pipelines are missing.
    bool render(rhi::CommandContext& ctx, const rhi::Texture& scene_color, const scene::PostProcess& settings);
    /// The bloom of the last render(); divide by levels() for an average.
    [[nodiscard]] const rhi::Texture& result() const noexcept { return levels_.front(); }
    [[nodiscard]] std::uint32_t levels() const noexcept { return used_; }

private:
    void ensure_levels(std::uint32_t width, std::uint32_t height, std::uint32_t count);
    struct Cached {
        rhi::Bindings bindings;
        const void* source = nullptr;
    };
    const rhi::Bindings& bindings(Cached& cached, const rhi::Pipeline& pipeline, const rhi::Texture& source);

    rhi::Device& device_;
    rhi::ShaderLibrary& shaders_;
    rhi::Pipeline down_pipeline_;
    rhi::Pipeline up_pipeline_;
    rhi::Buffer constants_;
    std::vector<rhi::Texture> levels_; ///< level 0: half the scene size
    std::vector<Cached> down_bindings_;
    std::vector<Cached> up_bindings_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t used_ = 0;
};

} // namespace f3d::render::raster
