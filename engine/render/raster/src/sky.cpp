// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/sky.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace f3d::render::raster {
namespace {

constexpr std::uint32_t kTransmittanceW = 256;
constexpr std::uint32_t kTransmittanceH = 64;
constexpr std::uint32_t kMultiScatterSize = 32;
constexpr std::uint32_t kSkyViewW = 192;
constexpr std::uint32_t kSkyViewH = 108;

std::uint32_t groups(std::uint32_t size) {
    return (size + 7) / 8;
}

/// Same atmosphere (everything the transmittance and multiple-scattering LUTs use)?
bool same_atmosphere(const SkyConstants& a, const SkyConstants& b) {
    return a.rayleigh_scattering == b.rayleigh_scattering && a.mie_scattering == b.mie_scattering &&
           a.mie_extinction == b.mie_extinction && a.ozone_absorption == b.ozone_absorption &&
           glm::vec3(a.planet) == glm::vec3(b.planet) && glm::vec3(a.ground_albedo) == glm::vec3(b.ground_albedo);
}

/// Same sky radiance (what the environment cube depends on)? The camera height is
/// compared in 50 m steps, so flying around doesn't re-bake every frame.
bool same_radiance(const SkyConstants& a, const SkyConstants& b) {
    if (!same_atmosphere(a, b) || a.ground_albedo.w != b.ground_albedo.w ||
        std::floor(a.planet.w * 20.0f) != std::floor(b.planet.w * 20.0f)) {
        return false;
    }
    // From sun_direction on, every field matters.
    const auto* pa = reinterpret_cast<const unsigned char*>(&a.sun_direction);
    const auto* pb = reinterpret_cast<const unsigned char*>(&b.sun_direction);
    const std::size_t bytes = sizeof(SkyConstants) - offsetof(SkyConstants, sun_direction);
    return std::memcmp(pa, pb, bytes) == 0;
}

rhi::Texture create_lut(rhi::Device& device, const char* name, std::uint32_t w, std::uint32_t h) {
    return device.create_texture(
        {name, w, h, rhi::Format::RGBA16Float, rhi::BindFlags::ShaderResource | rhi::BindFlags::UnorderedAccess});
}

} // namespace

SkyRenderer::SkyRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders) : device_(device), shaders_(shaders) {
    constants_ = device_.create_buffer(
        {"Sky constants", sizeof(SkyConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Default});
    filter_constants_ = device_.create_buffer(
        {"Filter constants", sizeof(FilterConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    sh_ = device_.create_buffer({"Environment SH", kMaxEnvSlots * 9 * sizeof(glm::vec4),
                                 rhi::BindFlags::ShaderResource | rhi::BindFlags::UnorderedAccess, rhi::Usage::Default,
                                 sizeof(glm::vec4)});
    transmittance_ = create_lut(device_, "Transmittance LUT", kTransmittanceW, kTransmittanceH);
    multiscatter_ = create_lut(device_, "Multiple-scattering LUT", kMultiScatterSize, kMultiScatterSize);
    sky_view_ = create_lut(device_, "Sky-view LUT", kSkyViewW, kSkyViewH);
    sky_source_ = create_source_cube("Sky environment", kSkyEnvSize, false);
    sky_specular_ = create_specular_cube("Sky specular", kSkyEnvSize);
}

rhi::Texture SkyRenderer::create_source_cube(const std::string& name, std::uint32_t size, bool render_target) const {
    rhi::TextureDesc desc;
    desc.name = name;
    desc.width = desc.height = size;
    desc.format = rhi::Format::RGBA16Float;
    desc.bind = rhi::BindFlags::ShaderResource |
                (render_target ? rhi::BindFlags::RenderTarget : rhi::BindFlags::UnorderedAccess);
    desc.mip_levels = 0; // full chain
    desc.dimension = rhi::TextureDimension::Cube;
    desc.generate_mips = true;
    return device_.create_texture(desc);
}

rhi::Texture SkyRenderer::create_specular_cube(const std::string& name, std::uint32_t size) const {
    rhi::TextureDesc desc;
    desc.name = name;
    desc.width = desc.height = size;
    desc.format = rhi::Format::RGBA16Float;
    desc.bind = rhi::BindFlags::ShaderResource | rhi::BindFlags::UnorderedAccess;
    desc.mip_levels = kSpecularMips;
    desc.dimension = rhi::TextureDimension::Cube;
    return device_.create_texture(desc);
}

void SkyRenderer::create_pipelines() {
    F3D_ZONE;
    shader_generation_ = shaders_.generation();
    const rhi::SamplerDesc clamp_transmittance{"g_TransmittanceLut", rhi::Filter::Linear, rhi::AddressMode::Clamp};
    const rhi::SamplerDesc clamp_multiscatter{"g_MultiScatterLut", rhi::Filter::Linear, rhi::AddressMode::Clamp};
    const rhi::SamplerDesc clamp_sky_view{"g_SkyViewLut", rhi::Filter::Linear, rhi::AddressMode::Clamp};
    const rhi::SamplerDesc clamp_source{"g_Source", rhi::Filter::Linear, rhi::AddressMode::Clamp};

    const auto compute = [&](const char* name, const char* shader, std::vector<rhi::SamplerDesc> samplers) {
        rhi::ComputePipelineDesc desc;
        desc.name = name;
        desc.cs = shaders_.load(shader);
        desc.samplers = std::move(samplers);
        return desc.cs ? device_.create_compute_pipeline(desc) : rhi::Pipeline{};
    };
    transmittance_pipeline_ = compute("Sky transmittance LUT", "sky_transmittance.cs", {});
    multiscatter_pipeline_ = compute("Sky multiple-scattering LUT", "sky_multiscatter.cs", {clamp_transmittance});
    sky_view_pipeline_ = compute("Sky-view LUT", "sky_view.cs", {clamp_transmittance, clamp_multiscatter});
    env_pipeline_ = compute("Sky environment cube", "sky_env.cs", {clamp_sky_view});
    prefilter_pipeline_ = compute("IBL prefilter", "ibl_prefilter.cs", {clamp_source});
    sh_pipeline_ = compute("IBL irradiance SH", "ibl_sh.cs", {clamp_source});

    rhi::GraphicsPipelineDesc sky;
    sky.name = "Sky";
    sky.vs = shaders_.load("sky.vs");
    sky.ps = shaders_.load("sky.ps");
    sky.cull = rhi::CullMode::None;
    sky.depth_test = true;
    sky.depth_write = false;
    sky.depth_func = rhi::CompareFunc::LessEqual;
    sky.color_formats = {rhi::Format::RGBA16Float};
    sky.depth_format = rhi::Format::D32Float;
    sky.samplers = {clamp_sky_view, clamp_transmittance};
    sky_pipeline_ = sky.vs && sky.ps ? device_.create_graphics_pipeline(sky) : rhi::Pipeline{};
    sky_bindings_ = {};
    sky_bindings_view_ = nullptr;
    luts_valid_ = false;
}

void SkyRenderer::set_filter(rhi::CommandContext& ctx, const FilterConstants& filter) {
    ctx.update_buffer(filter_constants_, &filter, sizeof(filter));
}

void SkyRenderer::build_luts(rhi::CommandContext& ctx) {
    F3D_ZONE;
    if (!transmittance_pipeline_ || !multiscatter_pipeline_) {
        return;
    }
    rhi::Bindings t = transmittance_pipeline_.create_bindings();
    t.set("SkyConstants", constants_);
    t.set_uav("g_OutLut", transmittance_);
    ctx.set_pipeline(transmittance_pipeline_);
    ctx.commit(t);
    ctx.dispatch(groups(kTransmittanceW), groups(kTransmittanceH));

    rhi::Bindings m = multiscatter_pipeline_.create_bindings();
    m.set("SkyConstants", constants_);
    m.set("g_TransmittanceLut", transmittance_);
    m.set_uav("g_OutLut", multiscatter_);
    ctx.set_pipeline(multiscatter_pipeline_);
    ctx.commit(m);
    ctx.dispatch(groups(kMultiScatterSize), groups(kMultiScatterSize));
    luts_valid_ = true;
}

void SkyRenderer::render_sky_view(rhi::CommandContext& ctx) {
    if (!sky_view_pipeline_) {
        return;
    }
    rhi::Bindings b = sky_view_pipeline_.create_bindings();
    b.set("SkyConstants", constants_);
    b.set("g_TransmittanceLut", transmittance_);
    b.set("g_MultiScatterLut", multiscatter_);
    b.set_uav("g_OutLut", sky_view_);
    ctx.set_pipeline(sky_view_pipeline_);
    ctx.commit(b);
    ctx.dispatch(groups(kSkyViewW), groups(kSkyViewH));
}

bool SkyRenderer::update(rhi::CommandContext& ctx, const SkyConstants& constants) {
    F3D_ZONE;
    if (shader_generation_ != shaders_.generation()) {
        create_pipelines();
        have_last_ = false;
    }
    const bool atmosphere_changed = !have_last_ || !same_atmosphere(constants, last_);
    const bool radiance_changed = !have_last_ || !same_radiance(constants, last_);
    physical_ = constants.ground_albedo.w > 0.5f;
    if (radiance_changed || !physical_ || std::memcmp(&constants, &last_, sizeof(constants)) != 0) {
        ctx.update_buffer(constants_, &constants, sizeof(constants));
    }
    last_ = constants;
    have_last_ = true;
    if (physical_) {
        rhi::ScopedGpuTimer timer(ctx, "sky_luts");
        if (atmosphere_changed || !luts_valid_) {
            build_luts(ctx);
        }
        render_sky_view(ctx);
    }
    return radiance_changed;
}

void SkyRenderer::bake_environment(rhi::CommandContext& ctx) {
    F3D_ZONE;
    if (!env_pipeline_) {
        return;
    }
    rhi::ScopedGpuTimer timer(ctx, "environment");
    set_filter(ctx, {glm::vec4(0.0f, static_cast<float>(kSkyEnvSize), 0.0f, 0.0f), glm::uvec4(0u)});
    rhi::Bindings b = env_pipeline_.create_bindings();
    b.set("SkyConstants", constants_);
    b.set("FilterConstants", filter_constants_);
    b.set("g_SkyViewLut", sky_view_);
    b.set("g_OutCube", sky_source_.view({rhi::ViewType::UnorderedAccess}));
    ctx.set_pipeline(env_pipeline_);
    ctx.commit(b);
    ctx.dispatch(groups(kSkyEnvSize), groups(kSkyEnvSize), 6);
    ctx.generate_mips(sky_source_);
    prefilter(ctx, sky_source_, sky_specular_);
    project_sh(ctx, sky_source_, 0);
}

void SkyRenderer::prefilter(rhi::CommandContext& ctx, const rhi::Texture& source, const rhi::Texture& target) {
    F3D_ZONE;
    if (!prefilter_pipeline_ || !source || !target) {
        return;
    }
    const auto source_size = static_cast<float>(source.width());
    const auto source_mips = static_cast<float>(source.mip_levels());
    ctx.set_pipeline(prefilter_pipeline_);
    for (std::uint32_t mip = 0; mip < std::min(kSpecularMips, target.mip_levels()); ++mip) {
        const std::uint32_t size = std::max(target.width() >> mip, 1u);
        const float roughness = static_cast<float>(mip) / static_cast<float>(kSpecularMips - 1);
        set_filter(ctx, {glm::vec4(roughness, static_cast<float>(size), source_size, source_mips), glm::uvec4(0u)});
        rhi::Bindings b = prefilter_pipeline_.create_bindings();
        b.set("FilterConstants", filter_constants_);
        b.set("g_Source", source);
        b.set("g_OutCube", target.view({rhi::ViewType::UnorderedAccess, mip}));
        ctx.commit(b);
        ctx.dispatch(groups(size), groups(size), 6);
    }
}

void SkyRenderer::project_sh(rhi::CommandContext& ctx, const rhi::Texture& source, std::uint32_t slot) {
    F3D_ZONE;
    if (!sh_pipeline_ || !source || slot >= kMaxEnvSlots) {
        return;
    }
    set_filter(ctx, {glm::vec4(0.0f, 0.0f, static_cast<float>(source.width()), static_cast<float>(source.mip_levels())),
                     glm::uvec4(slot, 0u, 0u, 0u)});
    rhi::Bindings b = sh_pipeline_.create_bindings();
    b.set("FilterConstants", filter_constants_);
    b.set("g_Source", source);
    b.set("g_SH", sh_);
    ctx.set_pipeline(sh_pipeline_);
    ctx.commit(b);
    ctx.dispatch(1);
}

void SkyRenderer::draw_sky(rhi::CommandContext& ctx, const rhi::Buffer& view_constants, const rhi::Buffer& exposure) {
    if (!sky_pipeline_ || !physical_) {
        return;
    }
    if (!sky_bindings_ || sky_bindings_view_ != view_constants.native() ||
        sky_bindings_exposure_ != exposure.native()) {
        sky_bindings_ = sky_pipeline_.create_bindings();
        sky_bindings_.set("ViewConstants", view_constants);
        sky_bindings_.set("g_Exposure", exposure);
        sky_bindings_exposure_ = exposure.native();
        sky_bindings_.set("SkyConstants", constants_);
        sky_bindings_.set("g_SkyViewLut", sky_view_);
        sky_bindings_.set("g_TransmittanceLut", transmittance_);
        sky_bindings_view_ = view_constants.native();
    }
    ctx.set_pipeline(sky_pipeline_);
    ctx.commit(sky_bindings_);
    ctx.draw(3);
}

} // namespace f3d::render::raster
