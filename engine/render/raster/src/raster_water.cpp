// SPDX-License-Identifier: Apache-2.0
// Water in the raster engine (phases.md P7.3): lakes (world::lake_surface), oceans (a
// grid around the camera) and rivers (River splines' water strips), drawn after the sky
// over a copy of the scene colour and depth (water.ps refracts and measures against the
// copy), before the fog.
#include "f3d/render/raster/raster_engine.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/terrain/builtin_textures.hpp"
#include "f3d/world/spline.hpp"
#include "f3d/world/water.hpp"

#include <algorithm>
#include <cmath>

namespace f3d::render::raster {
namespace {

constexpr float kOceanRadius = 12000.0f; ///< metres the ocean grid reaches from the camera
constexpr std::uint32_t kOceanGrid = 192; ///< quads per side (denser near the centre)

WaterConstants constants_for(const world::WaterBody& w, float time, float kind) {
    WaterConstants c;
    c.shallow = glm::vec4(w.shallow_color, std::max(w.clarity, 0.05f));
    c.deep = glm::vec4(w.deep_color, std::max(w.foam, 0.0f));
    c.waves = glm::vec4(std::max(w.wave_height, 0.0f), std::max(w.wave_length, 0.1f), std::max(w.wave_speed, 0.0f),
                        glm::radians(w.wind_direction));
    c.params = glm::vec4(std::max(w.refraction, 0.0f), std::clamp(w.roughness, 0.0f, 1.0f), time, kind);
    return c;
}

} // namespace

void RasterEngine::create_water_pipelines() {
    water_pipeline_ = {};
    water_copy_pipeline_ = {};
    water_bindings_ = {};
    water_copy_bindings_ = {};
    water_binding_key_ = {};
    water_copy_key_ = {};

    rhi::GraphicsPipelineDesc copy;
    copy.name = "Water copy";
    copy.vs = shaders_.load("fullscreen.vs");
    copy.ps = shaders_.load("water_copy.ps");
    copy.cull = rhi::CullMode::None;
    copy.depth_test = false;
    copy.depth_write = false;
    copy.color_formats = {color_format(), rhi::Format::R32Float};
    if (copy.vs && copy.ps) {
        water_copy_pipeline_ = device_.create_graphics_pipeline(copy);
    }

    rhi::GraphicsPipelineDesc water;
    water.name = "Water";
    water.vs = shaders_.load("water.vs");
    water.ps = shaders_.load("water.ps");
    water.vertex_layout = {{0, 0, rhi::Format::RGB32Float, 0}, {1, 1, rhi::Format::RG32Float, 0}};
    water.cull = rhi::CullMode::None; // seen from below too
    water.color_formats = {color_format()};
    water.depth_format = depth_format();
    water.samplers = {{"g_WaterColor", rhi::Filter::Linear, rhi::AddressMode::Clamp},
                      {"g_WaterDetail", rhi::Filter::Linear, rhi::AddressMode::Wrap},
                      {"g_EnvSpecular", rhi::Filter::Linear, rhi::AddressMode::Clamp},
                      {"g_ShadowMap", rhi::Filter::Linear, rhi::AddressMode::Clamp, rhi::CompareFunc::LessEqual}};
    if (water.vs && water.ps) {
        water_pipeline_ = device_.create_graphics_pipeline(water);
    }
    if (!water_constants_) {
        water_constants_ = device_.create_buffer(
            {"Water constants", sizeof(WaterConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    }
}

bool RasterEngine::prepare_water(float delta) {
    water_draws_.clear();
    if (!world_ || !water_pipeline_ || !water_copy_pipeline_) {
        return false;
    }
    F3D_ZONE;
    water_time_ = std::fmod(water_time_ + delta, 3600.0f);
    if (!water_detail_) {
        const core::Image detail = terrain::water_detail_texture(256);
        rhi::TextureDesc desc{"Water detail", detail.width, detail.height, rhi::Format::RGBA8Unorm,
                              rhi::BindFlags::ShaderResource | rhi::BindFlags::RenderTarget};
        desc.mip_levels = 0;
        desc.generate_mips = true;
        water_detail_ = device_.create_texture(desc);
        water_detail_needs_upload_ = true;
    }
    // A mesh from a strip: uploaded again when the strip object changes.
    const auto mesh_for = [&](const std::string& key,
                              const std::shared_ptr<const world::WaterStrip>& strip) -> const GpuWaterMesh* {
        if (!strip || strip->indices.empty()) {
            return nullptr;
        }
        GpuWaterMesh& gpu = water_meshes_[key];
        gpu.last_frame = draw_frame_;
        if (gpu.source.lock() != strip) {
            F3D_ZONE_N("Upload water mesh");
            gpu.positions = device_.create_buffer({"Water positions", strip->positions.size() * sizeof(glm::vec3),
                                                   rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                                                  strip->positions.data());
            gpu.flow = device_.create_buffer({"Water flow", strip->flow.size() * sizeof(glm::vec2),
                                              rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                                             strip->flow.data());
            gpu.indices = device_.create_buffer({"Water indices", strip->indices.size() * sizeof(std::uint32_t),
                                                 rhi::BindFlags::IndexBuffer, rhi::Usage::Immutable},
                                                strip->indices.data());
            gpu.index_count = static_cast<std::uint32_t>(strip->indices.size());
            gpu.source = strip;
        }
        return gpu.index_count > 0 ? &gpu : nullptr;
    };

    auto& registry = world_->registry();
    world_->update_transforms();
    for (const entt::entity e : registry.view<world::WaterBody>()) {
        const world::WaterBody& water = registry.get<world::WaterBody>(e);
        if (!water.visible || !scene::is_visible(*world_, e)) {
            continue;
        }
        if (water.kind == world::WaterKind::Ocean) {
            if (!ocean_grid_.index_count) {
                // A grid squeezed towards its centre: fine near the camera, coarse far away.
                std::vector<glm::vec3> positions;
                std::vector<glm::vec2> flow;
                std::vector<std::uint32_t> indices;
                for (std::uint32_t z = 0; z <= kOceanGrid; ++z) {
                    for (std::uint32_t x = 0; x <= kOceanGrid; ++x) {
                        const auto squeeze = [](std::uint32_t i) {
                            const float t = static_cast<float>(i) / static_cast<float>(kOceanGrid) * 2.0f - 1.0f;
                            return t * std::abs(t) * std::abs(t) * kOceanRadius;
                        };
                        positions.emplace_back(squeeze(x), 0.0f, squeeze(z));
                        flow.emplace_back(0.0f);
                    }
                }
                for (std::uint32_t z = 0; z < kOceanGrid; ++z) {
                    for (std::uint32_t x = 0; x < kOceanGrid; ++x) {
                        const std::uint32_t a = z * (kOceanGrid + 1) + x;
                        const std::uint32_t b = a + kOceanGrid + 1;
                        indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
                    }
                }
                ocean_grid_.positions = device_.create_buffer({"Ocean positions", positions.size() * sizeof(glm::vec3),
                                                               rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                                                              positions.data());
                ocean_grid_.flow = device_.create_buffer({"Ocean flow", flow.size() * sizeof(glm::vec2),
                                                          rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                                                         flow.data());
                ocean_grid_.indices = device_.create_buffer({"Ocean indices", indices.size() * sizeof(std::uint32_t),
                                                             rhi::BindFlags::IndexBuffer, rhi::Usage::Immutable},
                                                            indices.data());
                ocean_grid_.index_count = static_cast<std::uint32_t>(indices.size());
            }
            WaterDraw draw{&ocean_grid_, constants_for(water, water_time_, 2.0f)};
            // Snapped so the grid doesn't swim under the waves as the camera moves.
            const glm::vec2 centre = glm::round(glm::vec2(view_.camera_position.x, view_.camera_position.z) / 16.0f) * 16.0f;
            draw.constants.ocean = glm::vec4(centre, world::water_level(*world_, e), 0.0f);
            water_draws_.push_back(draw);
        } else if (const GpuWaterMesh* mesh =
                       mesh_for("lake:" + world_->guid(e).to_string(), world::lake_surface(*world_, e))) {
            water_draws_.push_back({mesh, constants_for(water, water_time_, 0.0f)});
        }
    }
    // Rivers: the look of a calm, clear stream.
    for (const entt::entity e : registry.view<world::Spline>()) {
        const world::Spline& spline = registry.get<world::Spline>(e);
        if (spline.kind != world::SplineKind::River || !spline.visible || !scene::is_visible(*world_, e)) {
            continue;
        }
        if (const GpuWaterMesh* mesh =
                mesh_for("river:" + world_->guid(e).to_string(), world::spline_geometry(*world_, e).water)) {
            world::WaterBody look = world::water_preset(world::WaterKind::Lake);
            look.clarity = 3.0f;
            look.wave_height = 0.0f;
            look.wave_length = 2.5f;
            look.foam = 0.5f;
            look.refraction = 0.7f;
            water_draws_.push_back({mesh, constants_for(look, water_time_, 1.0f)});
        }
    }
    std::erase_if(water_meshes_, [&](const auto& entry) { return entry.second.last_frame + 3 < draw_frame_; });
    return !water_draws_.empty();
}

void RasterEngine::copy_for_water(rhi::CommandContext& ctx, const rhi::Texture& color, const rhi::Texture& depth,
                                  const rhi::Texture& color_copy, const rhi::Texture& depth_copy) {
    if (water_detail_needs_upload_) {
        const core::Image detail = terrain::water_detail_texture(256);
        ctx.update_texture(water_detail_, 0, 0, detail.width, detail.height, detail.rgba.data(),
                           static_cast<std::size_t>(detail.width) * 4);
        ctx.generate_mips(water_detail_);
        water_detail_needs_upload_ = false;
    }
    const std::array<const void*, 2> key = {color.native(), depth.native()};
    if (key != water_copy_key_ || !water_copy_bindings_) {
        water_copy_bindings_ = water_copy_pipeline_.create_bindings();
        water_copy_bindings_.set("g_SceneColor", color);
        water_copy_bindings_.set("g_SceneDepth", depth);
        water_copy_key_ = key;
    }
    const std::array<rhi::Texture, 2> targets = {color_copy, depth_copy};
    ctx.set_render_targets(targets);
    ctx.set_pipeline(water_copy_pipeline_);
    ctx.commit(water_copy_bindings_);
    ctx.draw(3);
}

void RasterEngine::draw_water(rhi::CommandContext& ctx, const rhi::Texture& color_copy,
                              const rhi::Texture& depth_copy) {
    F3D_ZONE;
    const std::array<const void*, 4> key = {color_copy.native(), depth_copy.native(), sky_.environment().native(),
                                            shadow_map_.native()};
    if (key != water_binding_key_ || !water_bindings_) {
        water_bindings_ = water_pipeline_.create_bindings();
        water_bindings_.set("ViewConstants", view_constants_);
        water_bindings_.set("WaterConstants", water_constants_);
        water_bindings_.set("g_WaterColor", color_copy);
        water_bindings_.set("g_WaterDepth", depth_copy);
        water_bindings_.set("g_WaterDetail", water_detail_);
        water_bindings_.set("g_EnvSH", sky_.sh_buffer());
        water_bindings_.set("g_EnvSpecular", sky_.environment());
        water_bindings_.set("g_ShadowMap", shadow_map_);
        water_bindings_.set("g_Exposure", exposure_state_);
        water_binding_key_ = key;
    }
    ctx.set_pipeline(water_pipeline_);
    for (const WaterDraw& draw : water_draws_) {
        ctx.update_buffer(water_constants_, &draw.constants, sizeof(draw.constants));
        ctx.set_vertex_buffers({&draw.mesh->positions, &draw.mesh->flow});
        ctx.set_index_buffer(draw.mesh->indices);
        ctx.commit(water_bindings_);
        ctx.draw_indexed(draw.mesh->index_count, rhi::IndexType::UInt32);
    }
}

} // namespace f3d::render::raster
