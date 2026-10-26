// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/post_process.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <cmath>

namespace f3d::render::raster {

std::optional<scene::PostProcess> gather_post_process(scene::World& world) {
    for (const entt::entity e : world.depth_first()) {
        const auto* post = world.registry().try_get<scene::PostProcess>(e);
        if (post && post->visible && scene::is_visible(world, e)) {
            return *post;
        }
    }
    return std::nullopt;
}

std::uint32_t bloom_levels(float radius, std::uint32_t width, std::uint32_t height) noexcept {
    auto levels = static_cast<std::uint32_t>(std::lround(2.0f + std::clamp(radius, 0.0f, 1.0f) * 5.0f));
    // Level n is (size >> (n + 1)); keep the smallest at least 2 pixels.
    std::uint32_t size = std::min(width, height);
    std::uint32_t fit = 0;
    while (fit < levels && (size >> (fit + 1)) >= 2) {
        ++fit;
    }
    return std::max(fit, 1u);
}

BloomRenderer::BloomRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders) : device_(device), shaders_(shaders) {
    constants_ = device_.create_buffer(
        {"Bloom constants", sizeof(BloomConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
}

void BloomRenderer::create_pipelines() {
    rhi::GraphicsPipelineDesc desc;
    desc.vs = shaders_.load("fullscreen.vs");
    desc.cull = rhi::CullMode::None;
    desc.depth_test = false;
    desc.depth_write = false;
    desc.color_formats = {rhi::Format::RGBA16Float};
    desc.samplers.push_back({"g_Source", rhi::Filter::Linear, rhi::AddressMode::Clamp});
    desc.name = "Bloom downsample";
    desc.ps = shaders_.load("bloom_down.ps");
    down_pipeline_ = desc.vs && desc.ps ? device_.create_graphics_pipeline(desc) : rhi::Pipeline{};
    desc.name = "Bloom upsample";
    desc.ps = shaders_.load("bloom_up.ps");
    desc.blend = rhi::BlendMode::Additive;
    up_pipeline_ = desc.vs && desc.ps ? device_.create_graphics_pipeline(desc) : rhi::Pipeline{};
    down_bindings_.clear();
    up_bindings_.clear();
}

void BloomRenderer::ensure_levels(std::uint32_t width, std::uint32_t height, std::uint32_t count) {
    if (width_ == width && height_ == height && levels_.size() >= count) {
        return;
    }
    if (width_ != width || height_ != height) {
        levels_.clear();
    }
    width_ = width;
    height_ = height;
    for (auto n = static_cast<std::uint32_t>(levels_.size()); n < count; ++n) {
        rhi::TextureDesc desc;
        desc.name = "Bloom level " + std::to_string(n);
        desc.width = std::max(width >> (n + 1), 1u);
        desc.height = std::max(height >> (n + 1), 1u);
        desc.format = rhi::Format::RGBA16Float;
        desc.bind = rhi::BindFlags::ShaderResource | rhi::BindFlags::RenderTarget;
        levels_.push_back(device_.create_texture(desc));
    }
    down_bindings_.assign(levels_.size(), {});
    up_bindings_.assign(levels_.size(), {});
}

const rhi::Bindings& BloomRenderer::bindings(Cached& cached, const rhi::Pipeline& pipeline,
                                             const rhi::Texture& source) {
    if (!cached.bindings || cached.source != source.native()) {
        cached.bindings = pipeline.create_bindings();
        cached.bindings.set("BloomConstants", constants_);
        cached.bindings.set("g_Source", source);
        cached.source = source.native();
    }
    return cached.bindings;
}

bool BloomRenderer::render(rhi::CommandContext& ctx, const rhi::Texture& scene_color,
                           const scene::PostProcess& settings) {
    F3D_ZONE;
    if (!down_pipeline_ || !up_pipeline_) {
        return false;
    }
    used_ = bloom_levels(settings.bloom_radius, scene_color.width(), scene_color.height());
    ensure_levels(scene_color.width(), scene_color.height(), used_);
    const auto texel = [](const rhi::Texture& t) {
        return glm::vec2(1.0f / static_cast<float>(t.width()), 1.0f / static_cast<float>(t.height()));
    };
    const float threshold = settings.bloom_threshold > 0.0f ? 0.18f * std::exp2(settings.bloom_threshold) : 0.0f;
    // Down: scene -> level 0 -> level 1 -> ... (each target bound before its pipeline: the
    // scene colour may still be bound as a render target, now in the shader-resource state).
    for (std::uint32_t n = 0; n < used_; ++n) {
        const rhi::Texture& source = n == 0 ? scene_color : levels_[n - 1];
        const BloomConstants c{glm::vec4(texel(source), n == 0 ? 1.0f : 0.0f, threshold),
                               glm::vec4(texel(levels_[n]), 0.0f, 0.0f)};
        ctx.update_buffer(constants_, &c, sizeof(c));
        ctx.set_render_target(levels_[n]);
        ctx.set_pipeline(down_pipeline_);
        ctx.commit(bindings(down_bindings_[n], down_pipeline_, source));
        ctx.draw(3);
    }
    // Up: each level adds its (blurred) smaller neighbour, ending at level 0.
    for (std::uint32_t n = used_ - 1; n > 0; --n) {
        const rhi::Texture& source = levels_[n];
        const BloomConstants c{glm::vec4(texel(source), 0.0f, 0.0f),
                               glm::vec4(texel(levels_[n - 1]), 0.5f + settings.bloom_radius, 0.0f)};
        ctx.update_buffer(constants_, &c, sizeof(c));
        ctx.set_render_target(levels_[n - 1]);
        ctx.set_pipeline(up_pipeline_);
        ctx.commit(bindings(up_bindings_[n], up_pipeline_, source));
        ctx.draw(3);
    }
    return true;
}

} // namespace f3d::render::raster
