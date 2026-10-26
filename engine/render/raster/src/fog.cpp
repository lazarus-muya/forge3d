// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/fog.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <cmath>

namespace f3d::render::raster {
namespace {

/// History weight of the volumetric fog's temporal accumulation.
constexpr float kHistoryWeight = 0.9f;
/// The sky's distance when the HeightFog doesn't set one (m).
constexpr float kDefaultSkyDistance = 30000.0f;
/// The froxel volume's depth without a HeightFog (fog volumes only).
constexpr float kDefaultVolumetricDistance = 150.0f;

} // namespace

SceneFog gather_fog(scene::World& world) {
    F3D_ZONE;
    SceneFog out;
    world.update_transforms();
    for (const entt::entity e : world.depth_first()) {
        const auto& registry = world.registry();
        if (const auto* fog = registry.try_get<scene::HeightFog>(e);
            fog && fog->visible && !out.height && scene::is_visible(world, e)) {
            out.height = *fog;
            out.base_height = world.transform(e).world[3].y;
        }
        const auto* volume = registry.try_get<scene::FogVolume>(e);
        if (!volume || !volume->visible || out.volumes.size() >= kMaxFogVolumes || !scene::is_visible(world, e)) {
            continue;
        }
        const glm::mat4& world_matrix = world.transform(e).world;
        const glm::vec3 size = glm::max(volume->size, glm::vec3(1e-3f));
        const glm::mat4 box = world_matrix * glm::scale(glm::mat4(1.0f), size);
        FogVolumeGpu gpu;
        gpu.world_to_box = glm::inverse(box);
        gpu.medium = glm::vec4(glm::clamp(volume->albedo, 0.0f, 1.0f), std::max(volume->density, 0.0f));
        gpu.shape = glm::vec4(std::max(volume->height_falloff, 0.0f), glm::length(glm::vec3(box[1])),
                              std::clamp(volume->edge_fade, 0.0f, 1.0f), 0.0f);
        gpu.emissive = glm::vec4(glm::max(volume->emissive, glm::vec3(0.0f)), 0.0f);
        out.volumes.push_back(gpu);
    }
    return out;
}

float height_fog_optical_depth(const scene::HeightFog& fog, float base_height, const glm::vec3& origin,
                               const glm::vec3& dir, float s0, float s1) noexcept {
    if (s1 <= s0) {
        return 0.0f;
    }
    const float k = fog.height_falloff;
    const float base = fog.density * std::exp(std::clamp(-k * (origin.y - base_height), -80.0f, 80.0f));
    const float kv = k * dir.y;
    const float integral = std::abs(kv) > 1e-5f ? (std::exp(std::clamp(-kv * s0, -80.0f, 80.0f)) -
                                                   std::exp(std::clamp(-kv * s1, -80.0f, 80.0f))) /
                                                      kv
                                                : s1 - s0;
    return base * std::max(integral, 0.0f);
}

glm::uvec3 fog_grid_size(std::uint32_t width, std::uint32_t height, FogQuality quality) noexcept {
    std::uint32_t tile = 0;
    std::uint32_t slices = 0;
    switch (quality) {
    case FogQuality::Off:
        return glm::uvec3(0u);
    case FogQuality::Low:
        tile = 16;
        slices = 48;
        break;
    case FogQuality::Medium:
        tile = 12;
        slices = 64;
        break;
    case FogQuality::High:
        tile = 8;
        slices = 64;
        break;
    case FogQuality::Epic:
        tile = 6;
        slices = 96;
        break;
    }
    return {std::max((width + tile - 1) / tile, 1u), std::max((height + tile - 1) / tile, 1u), slices};
}

FogRenderer::FogRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders) : device_(device), shaders_(shaders) {
    constants_ = device_.create_buffer(
        {"Fog constants", sizeof(FogConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    rhi::TextureDesc empty;
    empty.name = "Fog (empty volume)";
    empty.format = rhi::Format::RGBA16Float;
    empty.bind = rhi::BindFlags::ShaderResource | rhi::BindFlags::UnorderedAccess;
    empty.dimension = rhi::TextureDimension::Tex3D;
    empty_volume_ = device_.create_texture(empty);
}

void FogRenderer::create_pipelines() {
    const auto compute = [&](const char* name, const char* shader, bool history) {
        rhi::ComputePipelineDesc desc;
        desc.name = name;
        desc.cs = shaders_.load(shader);
        if (history) {
            desc.samplers.push_back({"g_FogHistory", rhi::Filter::Linear, rhi::AddressMode::Clamp});
            desc.samplers.push_back(
                {"g_ShadowMap", rhi::Filter::Linear, rhi::AddressMode::Clamp, rhi::CompareFunc::LessEqual});
        }
        return desc.cs ? device_.create_compute_pipeline(desc) : rhi::Pipeline{};
    };
    inject_pipeline_ = compute("Fog inject", "fog_inject.cs", true);
    integrate_pipeline_ = compute("Fog integrate", "fog_integrate.cs", false);

    rhi::GraphicsPipelineDesc apply;
    apply.name = "Fog apply";
    apply.vs = shaders_.load("fullscreen.vs");
    apply.ps = shaders_.load("fog_apply.ps");
    apply.cull = rhi::CullMode::None;
    apply.depth_test = false;
    apply.depth_write = false;
    apply.blend = rhi::BlendMode::PremultipliedAlpha;
    apply.color_formats = {rhi::Format::RGBA16Float};
    apply.samplers.push_back({"g_FogIntegrated", rhi::Filter::Linear, rhi::AddressMode::Clamp});
    apply_pipeline_ = apply.vs && apply.ps ? device_.create_graphics_pipeline(apply) : rhi::Pipeline{};

    inject_bindings_ = {};
    integrate_bindings_ = {};
    apply_bindings_ = {};
}

void FogRenderer::ensure_volumes(const glm::uvec3& size) {
    if (grid_ == size && integrated_) {
        return;
    }
    grid_ = size;
    rhi::TextureDesc desc;
    desc.format = rhi::Format::RGBA16Float;
    desc.bind = rhi::BindFlags::ShaderResource | rhi::BindFlags::UnorderedAccess;
    desc.dimension = rhi::TextureDimension::Tex3D;
    desc.width = size.x;
    desc.height = size.y;
    desc.array_size = size.z;
    desc.name = "Fog scattering A";
    scatter_[0] = device_.create_texture(desc);
    desc.name = "Fog scattering B";
    scatter_[1] = device_.create_texture(desc);
    desc.name = "Fog integrated";
    integrated_ = device_.create_texture(desc);
    history_valid_ = false;
}

bool FogRenderer::prepare(rhi::CommandContext& ctx, const View& view, scene::World* world,
                          const std::vector<DirectionalLight>& directional, FogQuality quality, bool sky_background) {
    volumetric_ = false;
    if (!world || !apply_pipeline_) {
        return false;
    }
    const SceneFog fog = gather_fog(*world);
    if (fog.empty()) {
        history_valid_ = false;
        return false;
    }
    const glm::uvec3 grid = fog_grid_size(view.width, view.height, quality);
    volumetric_ = fog.volumetric() && grid.z > 0 && inject_pipeline_ && integrate_pipeline_;

    FogConstants c;
    const scene::HeightFog settings = fog.height.value_or(scene::HeightFog{});
    if (fog.height) {
        c.height = glm::vec4(std::max(settings.density, 0.0f), std::max(settings.height_falloff, 0.0f),
                             fog.base_height, 1.0f);
    }
    c.albedo = glm::vec4(glm::clamp(settings.albedo, 0.0f, 1.0f), std::clamp(settings.anisotropy, -0.95f, 0.95f));
    c.light = glm::vec4(std::max(settings.sky_lighting, 0.0f), std::max(settings.sun_lighting, 0.0f),
                        std::max(settings.start_distance, 0.0f), std::clamp(settings.max_opacity, 0.0f, 1.0f));
    const float distance = fog.height ? std::max(settings.volumetric_distance, 1.0f) : kDefaultVolumetricDistance;
    c.range = glm::vec4(volumetric_ ? distance : 0.0f,
                        settings.cutoff_distance > 0.0f ? settings.cutoff_distance : kDefaultSkyDistance,
                        fog.height ? std::max(settings.volumetric_density, 0.0f) : 0.0f, 0.0f);
    for (std::size_t i = 0; i < directional.size() && i < 4; ++i) {
        c.dir_scatter[static_cast<glm::length_t>(i)] = std::max(directional[i].volumetric, 0.0f);
    }
    c.counts = glm::uvec4(static_cast<std::uint32_t>(fog.volumes.size()), sky_background ? 1u : 0u, 0u, 0u);
    std::copy(fog.volumes.begin(), fog.volumes.end(), c.volumes);

    if (volumetric_) {
        ensure_volumes(grid);
        // The history is usable if the fog itself didn't change (a moved camera is reprojected).
        if (!(prev_fog_ == fog.height)) {
            history_valid_ = false;
        }
        ++frame_;
        current_ = static_cast<std::uint32_t>(frame_ & 1u);
        // R2 sequence: well-spread depth offsets over consecutive frames.
        const float jitter = std::fmod(0.5f + static_cast<float>(frame_ % 1024u) * 0.6180339887f, 1.0f);
        c.grid = glm::vec4(glm::vec3(grid), jitter);
        // R2 in the plane too: the history averages the whole cell, not its centre.
        const auto n = static_cast<float>(frame_ % 1024u);
        c.jitter = glm::vec4(std::fmod(n * 0.7548776662f, 1.0f) - 0.5f, std::fmod(n * 0.5698402910f, 1.0f) - 0.5f,
                             0.0f, 0.0f);
        c.range.w = history_valid_ ? kHistoryWeight : 0.0f;
        c.prev_view = prev_view_row_;
        c.prev_view_projection = prev_view_projection_;
        prev_view_projection_ = view.view_projection();
        prev_view_row_ = glm::vec4(view.view[0][2], view.view[1][2], view.view[2][2], view.view[3][2]);
        history_valid_ = true;
    } else {
        history_valid_ = false;
    }
    prev_fog_ = fog.height;
    ctx.update_buffer(constants_, &c, sizeof(c));
    return true;
}

rhi::Bindings& FogRenderer::bindings(Cached& cached, const rhi::Pipeline& pipeline,
                                     const std::array<const void*, 10>& key, bool& fresh) {
    fresh = !cached.bindings || cached.key != key;
    if (fresh) {
        cached.bindings = pipeline.create_bindings();
        cached.key = key;
    }
    return cached.bindings;
}

void FogRenderer::compute(rhi::CommandContext& ctx, const Inputs& in) {
    F3D_ZONE;
    if (!volumetric_) {
        return;
    }
    const rhi::Texture& out = scatter_[current_];
    const rhi::Texture& history = scatter_[current_ ^ 1u];
    bool fresh = false;
    rhi::Bindings& inject = bindings(inject_bindings_[current_], inject_pipeline_,
                                     {in.view_constants->native(), in.lights->native(), in.cluster_counts->native(),
                                      in.cluster_indices->native(), in.env_sh->native(), in.shadow_map->native(),
                                      in.exposure->native(), out.native(), history.native(), constants_.native()},
                                     fresh);
    if (fresh) {
        inject.set("ViewConstants", *in.view_constants);
        inject.set("FogConstants", constants_);
        inject.set("g_Lights", *in.lights);
        inject.set("g_ClusterCounts", *in.cluster_counts);
        inject.set("g_ClusterIndices", *in.cluster_indices);
        inject.set("g_EnvSH", *in.env_sh);
        inject.set("g_ShadowMap", *in.shadow_map);
        inject.set("g_Exposure", *in.exposure);
        inject.set("g_FogHistory", history);
        inject.set_uav("g_FogOut", out);
    }
    ctx.set_pipeline(inject_pipeline_);
    ctx.commit(inject);
    ctx.dispatch((grid_.x + 3) / 4, (grid_.y + 3) / 4, (grid_.z + 3) / 4);

    rhi::Bindings& integrate = bindings(integrate_bindings_, integrate_pipeline_,
                                        {in.view_constants->native(), out.native(), integrated_.native(),
                                         constants_.native(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
                                        fresh);
    if (fresh) {
        integrate.set("ViewConstants", *in.view_constants);
        integrate.set("FogConstants", constants_);
        integrate.set("g_FogScatter", out);
        integrate.set_uav("g_FogIntegrated", integrated_);
    }
    ctx.set_pipeline(integrate_pipeline_);
    ctx.commit(integrate);
    ctx.dispatch((grid_.x + 7) / 8, (grid_.y + 7) / 8);
}

void FogRenderer::apply(rhi::CommandContext& ctx, const Inputs& in, const rhi::Texture& depth) {
    F3D_ZONE;
    const rhi::Texture& volume = volumetric_ ? integrated_ : empty_volume_;
    bool fresh = false;
    rhi::Bindings& b = bindings(apply_bindings_, apply_pipeline_,
                                {in.view_constants->native(), in.env_sh->native(), in.exposure->native(),
                                 depth.native(), volume.native(), constants_.native(), nullptr, nullptr, nullptr,
                                 nullptr},
                                fresh);
    if (fresh) {
        b.set("ViewConstants", *in.view_constants);
        b.set("FogConstants", constants_);
        b.set("g_EnvSH", *in.env_sh);
        b.set("g_Exposure", *in.exposure);
        b.set("g_SceneDepth", depth);
        b.set("g_FogIntegrated", volume);
    }
    ctx.set_pipeline(apply_pipeline_);
    ctx.commit(b);
    ctx.draw(3);
}

} // namespace f3d::render::raster
