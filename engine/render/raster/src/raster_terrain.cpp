// SPDX-License-Identifier: Apache-2.0
// Terrain drawing in the raster engine (phases.md P6.1, P6.4): GPU height and splat
// textures updated per changed tile, CDLOD node selection each frame, and instanced
// grid draws in the lit, wireframe, shadow and pick passes. terrain.ps blends the
// material layers and shades like mesh.ps; pick.ps writes the ID buffer.
#include "f3d/render/raster/raster_engine.hpp"

#include "f3d/assets/mesh_library.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/terrain/builtin_textures.hpp"
#include "f3d/terrain/terrain.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace f3d::render::raster {
namespace {

constexpr std::uint32_t kGrid = terrain::kGridQuads;
constexpr std::uint32_t kHalf = kGrid / 2;

/// Grid vertices (0..32 per side) and indices ordered by quadrant: the first quarter
/// of the indices covers grid quads [0, 16)^2, which draws a quarter node.
void make_grid(std::vector<glm::vec2>& vertices, std::vector<std::uint32_t>& indices) {
    for (std::uint32_t z = 0; z <= kGrid; ++z) {
        for (std::uint32_t x = 0; x <= kGrid; ++x) {
            vertices.emplace_back(static_cast<float>(x), static_cast<float>(z));
        }
    }
    const auto at = [](std::uint32_t x, std::uint32_t z) { return z * (kGrid + 1) + x; };
    for (std::uint32_t q = 0; q < 4; ++q) {
        const std::uint32_t qx = (q & 1) * kHalf;
        const std::uint32_t qz = (q >> 1) * kHalf;
        for (std::uint32_t z = qz; z < qz + kHalf; ++z) {
            for (std::uint32_t x = qx; x < qx + kHalf; ++x) {
                // Counter-clockwise seen from above (+Y).
                indices.insert(indices.end(), {at(x, z), at(x, z + 1), at(x + 1, z)});
                indices.insert(indices.end(), {at(x + 1, z), at(x, z + 1), at(x + 1, z + 1)});
            }
        }
    }
}

} // namespace

void RasterEngine::create_terrain_pipelines(const std::vector<rhi::VertexAttribute>&) {
    const std::vector<rhi::VertexAttribute> layout = {{0, 0, rhi::Format::RG32Float, 0}};
    const rhi::SamplerDesc height_sampler{"g_TerrainHeight", rhi::Filter::Linear, rhi::AddressMode::Clamp};
    rhi::GraphicsPipelineDesc lit;
    lit.vs = shaders_.load("terrain.vs");
    lit.ps = shaders_.load("terrain.ps");
    lit.vertex_layout = layout;
    lit.samplers.push_back({"g_TerrainSplat", rhi::Filter::Linear, rhi::AddressMode::Clamp});
    lit.samplers.push_back({"g_LayerAlbedo", rhi::Filter::Anisotropic, rhi::AddressMode::Wrap});
    lit.samplers.push_back({"g_LayerNormal", rhi::Filter::Anisotropic, rhi::AddressMode::Wrap});
    lit.samplers.push_back({"g_EnvSpecular", rhi::Filter::Linear, rhi::AddressMode::Clamp});
    lit.samplers.push_back({"g_ShadowMap", rhi::Filter::Linear, rhi::AddressMode::Clamp, rhi::CompareFunc::LessEqual});
    lit.samplers.push_back(height_sampler);
    lit.color_formats = {color_format()};
    lit.depth_format = depth_format();
    lit.cull = rhi::CullMode::Back;
    terrain_pipelines_ = {};
    if (!lit.vs || !lit.ps) {
        return;
    }
    lit.name = "Terrain";
    terrain_pipelines_[static_cast<std::size_t>(TerrainPass::Lit)] = device_.create_graphics_pipeline(lit);
    rhi::GraphicsPipelineDesc wire = lit;
    wire.name = "Terrain (wireframe)";
    wire.wireframe = true;
    wire.cull = rhi::CullMode::None;
    terrain_pipelines_[static_cast<std::size_t>(TerrainPass::Wireframe)] = device_.create_graphics_pipeline(wire);

    rhi::GraphicsPipelineDesc shadow;
    shadow.name = "Terrain (shadow casters)";
    shadow.vs = lit.vs;
    shadow.vertex_layout = layout;
    shadow.samplers = {height_sampler};
    shadow.cull = rhi::CullMode::None;
    shadow.slope_scaled_depth_bias = 1.5f;
    shadow.depth_clip = false;
    shadow.depth_format = rhi::Format::D32Float;
    terrain_pipelines_[static_cast<std::size_t>(TerrainPass::Shadow)] = device_.create_graphics_pipeline(shadow);

    rhi::GraphicsPipelineDesc pick;
    pick.name = "Terrain (pick)";
    pick.vs = lit.vs;
    pick.ps = shaders_.load("pick.ps");
    pick.vertex_layout = layout;
    pick.samplers = {height_sampler};
    pick.cull = rhi::CullMode::None;
    pick.color_formats = {rhi::Format::R32Uint};
    pick.depth_format = depth_format();
    if (pick.ps) {
        terrain_pipelines_[static_cast<std::size_t>(TerrainPass::Pick)] = device_.create_graphics_pipeline(pick);
    }

    if (!terrain_grid_vertices_) {
        std::vector<glm::vec2> vertices;
        std::vector<std::uint32_t> indices;
        make_grid(vertices, indices);
        terrain_grid_vertices_ = device_.create_buffer({"Terrain grid vertices", vertices.size() * sizeof(glm::vec2),
                                                        rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                                                       vertices.data());
        terrain_grid_indices_ = device_.create_buffer({"Terrain grid indices", indices.size() * sizeof(std::uint32_t),
                                                       rhi::BindFlags::IndexBuffer, rhi::Usage::Immutable},
                                                      indices.data());
        terrain_full_indices_ = static_cast<std::uint32_t>(indices.size());
        terrain_quarter_indices_ = terrain_full_indices_ / 4;
        terrain_constants_ = device_.create_buffer(
            {"Terrain constants", sizeof(TerrainConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
        terrain_layer_constants_ = device_.create_buffer({"Terrain layer constants", sizeof(TerrainLayerConstants),
                                                          rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    }
    for (auto& [guid, gpu] : terrains_) {
        gpu.bindings = {}; // made for the old pipelines
    }
}

void RasterEngine::prepare_terrains(rhi::CommandContext& ctx, const View& view) {
    last_terrain_nodes_ = 0;
    if (!world_ || !terrain_pipelines_[0]) {
        return;
    }
    F3D_ZONE;
    for (auto& [guid, gpu] : terrains_) {
        gpu.seen = false;
    }
    auto& registry = world_->registry();
    for (const entt::entity e : registry.view<terrain::Terrain>()) {
        if (!scene::is_visible(*world_, e)) {
            continue;
        }
        const auto hf = terrain::surface_of(*world_, e); // with the splines' roads and riverbeds
        if (!hf) {
            continue; // no heights (e.g. a file saved before terrains were persisted)
        }
        const terrain::Terrain& t = registry.get<terrain::Terrain>(e);
        GpuTerrain& gpu = terrains_[world_->guid(e)];
        gpu.seen = true;
        gpu.last_frame = draw_frame_;

        // Heights: a texture of the terrain's format; changed tiles are re-uploaded.
        const rhi::Format format =
            t.format == terrain::HeightFormat::R16 ? rhi::Format::R16Unorm : rhi::Format::R32Float;
        const float scale = t.max_height - t.min_height;
        const auto encode_row = [&](std::uint32_t z, std::uint32_t x0, std::uint32_t count,
                                    std::vector<std::byte>& out) {
            const float* src = hf->heights().data() + static_cast<std::size_t>(z) * hf->samples() + x0;
            if (format == rhi::Format::R16Unorm) {
                const std::size_t base = out.size();
                out.resize(base + count * 2);
                auto* dst = reinterpret_cast<std::uint16_t*>(out.data() + base);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const float n = scale > 0.0f ? (src[i] - t.min_height) / scale : 0.0f;
                    dst[i] = static_cast<std::uint16_t>(std::lround(std::clamp(n, 0.0f, 1.0f) * 65535.0f));
                }
            } else {
                const std::size_t base = out.size();
                out.resize(base + count * 4);
                std::memcpy(out.data() + base, src, count * 4);
            }
        };
        const std::uint32_t n = hf->samples();
        const std::size_t texel = format == rhi::Format::R16Unorm ? 2 : 4;
        const bool new_source = gpu.source != hf.get(); // the surface was (re)created or dropped
        gpu.source = hf.get();
        if (new_source) {
            gpu.splat = {};
        }
        if (!gpu.heights || gpu.samples != n || gpu.format != format || new_source) {
            F3D_ZONE_N("Upload terrain heights");
            std::vector<std::byte> data;
            data.reserve(static_cast<std::size_t>(n) * n * texel);
            for (std::uint32_t z = 0; z < n; ++z) {
                encode_row(z, 0, n, data);
                data.resize((data.size() + 3) & ~std::size_t{3}); // rows 4-byte aligned
            }
            rhi::TextureDesc desc{"Terrain heights", n, n, format, rhi::BindFlags::ShaderResource};
            gpu.heights = device_.create_texture(desc);
            ctx.update_texture(gpu.heights, 0, 0, n, n, data.data(), (n * texel + 3) & ~std::size_t{3});
            gpu.samples = n;
            gpu.format = format;
            gpu.bindings = {};
            (void)hf->take_dirty_tiles();
            gpu.revision = hf->revision();
        } else if (gpu.revision != hf->revision()) {
            F3D_ZONE_N("Update terrain tiles");
            for (const std::uint32_t tile : hf->take_dirty_tiles()) {
                const std::uint32_t tx = (tile % hf->tiles()) * terrain::kTileQuads;
                const std::uint32_t tz = (tile / hf->tiles()) * terrain::kTileQuads;
                const std::uint32_t size = terrain::kTileQuads + 1;
                std::vector<std::byte> data;
                data.reserve(static_cast<std::size_t>(size) * size * texel);
                for (std::uint32_t z = tz; z < tz + size; ++z) {
                    encode_row(z, tx, size, data);
                    data.resize((data.size() + 3) & ~std::size_t{3});
                }
                ctx.update_texture(gpu.heights, tx, tz, size, size, data.data(), (size * texel + 3) & ~std::size_t{3});
            }
            gpu.revision = hf->revision();
        }

        // Layer weights: a texture array (one RGBA8 slice per 4 layers); changed tiles
        // are re-uploaded. Without splat maps, a 1x1 texture of layer 0.
        const terrain::Splatmap* splat = hf->splat();
        if (!splat) {
            if (!gpu.splat || gpu.splat_maps != 0) {
                rhi::TextureDesc desc{"Terrain splat (default)", 1, 1, rhi::Format::RGBA8Unorm,
                                      rhi::BindFlags::ShaderResource};
                desc.dimension = rhi::TextureDimension::Tex2DArray;
                gpu.splat = device_.create_texture(desc);
                const std::uint8_t layer0[4] = {255, 0, 0, 0};
                ctx.update_texture(gpu.splat, 0, 0, 1, 1, layer0, 4);
                gpu.splat_maps = 0;
                gpu.splat_resolution = 1;
            }
        } else if (!gpu.splat || gpu.splat_maps != splat->maps() || gpu.splat_resolution != splat->resolution()) {
            F3D_ZONE_N("Upload terrain splat");
            const std::uint32_t res = splat->resolution();
            rhi::TextureDesc desc{"Terrain splat", res, res, rhi::Format::RGBA8Unorm, rhi::BindFlags::ShaderResource};
            desc.dimension = rhi::TextureDimension::Tex2DArray;
            desc.array_size = splat->maps();
            gpu.splat = device_.create_texture(desc);
            for (std::uint32_t m = 0; m < splat->maps(); ++m) {
                ctx.update_texture(gpu.splat, 0, 0, res, res, splat->map(m), static_cast<std::size_t>(res) * 4, m);
            }
            gpu.splat_maps = splat->maps();
            gpu.splat_resolution = res;
            (void)hf->splat()->take_dirty_tiles();
            gpu.splat_revision = splat->revision();
        } else if (gpu.splat_revision != splat->revision()) {
            F3D_ZONE_N("Update terrain splat tiles");
            const std::uint32_t res = splat->resolution();
            for (const std::uint32_t tile : hf->splat()->take_dirty_tiles()) {
                const std::uint32_t tx = (tile % splat->tiles()) * terrain::kSplatPerTile;
                const std::uint32_t tz = (tile / splat->tiles()) * terrain::kSplatPerTile;
                for (std::uint32_t m = 0; m < splat->maps(); ++m) {
                    const std::uint8_t* first = splat->map(m) + (static_cast<std::size_t>(tz) * res + tx) * 4;
                    ctx.update_texture(gpu.splat, tx, tz, terrain::kSplatPerTile, terrain::kSplatPerTile, first,
                                       static_cast<std::size_t>(res) * 4, m);
                }
            }
            gpu.splat_revision = splat->revision();
        }

        update_layer_textures(ctx, gpu, t);

        // LOD selection: frustum-culled for the camera; the whole terrain for shadows.
        const glm::vec3 origin = terrain::terrain_origin(*world_, e);
        const glm::vec3 camera = view.camera_position - origin;
        const glm::mat4 local_vp = view.view_projection() * glm::translate(glm::mat4(1.0f), origin);
        const auto planes = terrain::frustum_planes(local_vp);
        const terrain::LodSelection camera_sel = terrain::select_lods(*hf, t, camera, &planes);
        const terrain::LodSelection shadow_sel =
            frame_.shadow_light >= 0 ? terrain::select_lods(*hf, t, camera) : terrain::LodSelection{};
        std::vector<glm::uvec4> nodes;
        const auto append = [&](const terrain::LodSelection& s, std::uint32_t& full, std::uint32_t& quarter) {
            full = quarter = 0;
            for (const terrain::LodNode& node : s.nodes) {
                if (node.quads == (kGrid << node.lod)) {
                    nodes.emplace_back(node.x, node.z, node.lod, node.quads);
                    ++full;
                }
            }
            for (const terrain::LodNode& node : s.nodes) {
                if (node.quads != (kGrid << node.lod)) {
                    nodes.emplace_back(node.x, node.z, node.lod, node.quads);
                    ++quarter;
                }
            }
        };
        append(camera_sel, gpu.camera_full, gpu.camera_quarter);
        append(shadow_sel, gpu.shadow_full, gpu.shadow_quarter);
        last_terrain_nodes_ += gpu.camera_full + gpu.camera_quarter;
        const auto count = static_cast<std::uint32_t>(std::max<std::size_t>(nodes.size(), 1));
        if (!gpu.nodes || count > gpu.node_capacity) {
            gpu.node_capacity = std::max(count * 2, 256u);
            gpu.nodes =
                device_.create_buffer({"Terrain nodes", gpu.node_capacity * sizeof(glm::uvec4),
                                       rhi::BindFlags::ShaderResource, rhi::Usage::Default, sizeof(glm::uvec4)});
            gpu.bindings = {};
        }
        if (!nodes.empty()) {
            ctx.update_buffer(gpu.nodes, nodes.data(), nodes.size() * sizeof(glm::uvec4));
        }

        TerrainConstants& c = gpu.constants;
        c.origin = glm::vec4(origin, hf->spacing());
        c.params =
            glm::vec4(static_cast<float>(n), hf->size(), t.min_height, format == rhi::Format::R16Unorm ? scale : 0.0f);
        c.camera = glm::vec4(camera, 0.0f);
        for (std::uint32_t l = 0; l < 12; ++l) {
            const float end = l < camera_sel.lod_count ? camera_sel.ranges[l] : 1.0e30f;
            const float start = end * 0.7f;
            c.morph[l] = glm::vec4(start, 1.0f / std::max(end - start, 1e-3f), 0.0f, 0.0f);
        }
        // Every pass's bindings are made once per (re)created resource set.
        const std::array<const void*, 7> key = {lights_.native(),          shadow_map_.native(),
                                                sky_.environment().native(), gpu.nodes.native(),
                                                gpu.splat.native(),          gpu.layer_albedo.native(),
                                                gpu.layer_normal.native()};
        if (key != gpu.binding_key) {
            gpu.bindings = {};
            gpu.binding_key = key;
        }
        if (!gpu.bindings[0]) {
            for (std::size_t p = 0; p < gpu.bindings.size(); ++p) {
                if (!terrain_pipelines_[p]) {
                    continue;
                }
                rhi::Bindings b = terrain_pipelines_[p].create_bindings();
                b.set("TerrainConstants", terrain_constants_);
                b.set("g_TerrainHeight", gpu.heights);
                b.set("g_TerrainNodes", gpu.nodes);
                if (p == static_cast<std::size_t>(TerrainPass::Lit) ||
                    p == static_cast<std::size_t>(TerrainPass::Wireframe)) {
                    b.set("ViewConstants", view_constants_);
                    b.set("TerrainLayerConstants", terrain_layer_constants_);
                    b.set("g_TerrainSplat", gpu.splat);
                    b.set("g_LayerAlbedo", gpu.layer_albedo);
                    b.set("g_LayerNormal", gpu.layer_normal);
                    b.set("g_Lights", lights_);
                    b.set("g_ClusterCounts", cluster_counts_);
                    b.set("g_ClusterIndices", cluster_indices_);
                    b.set("g_EnvSH", sky_.sh_buffer());
                    b.set("g_EnvSpecular", sky_.environment());
                    b.set("g_ShadowMap", shadow_map_);
                    b.set("g_Exposure", exposure_state_);
                } else if (p == static_cast<std::size_t>(TerrainPass::Pick)) {
                    b.set("ObjectConstants", object_constants_);
                }
                gpu.bindings[p] = b;
            }
        }
    }
    std::erase_if(terrains_,
                  [&](const auto& entry) { return !entry.second.seen && entry.second.last_frame + 3 < draw_frame_; });
}

const std::vector<std::uint8_t>* RasterEngine::layer_image(const core::AssetRef& ref, std::uint32_t size) {
    if (ref.empty()) {
        return nullptr;
    }
    const std::string key = ref.guid.to_string() + "|" + ref.path + "|" + std::to_string(size);
    if (const auto it = layer_images_.find(key); it != layer_images_.end()) {
        return it->second.empty() ? nullptr : &it->second;
    }
    std::vector<std::uint8_t>& out = layer_images_[key];
    F3D_ZONE_N("Load terrain layer image");
    std::optional<core::Image> image = terrain::builtin_texture(ref.path);
    if (!image && meshes_) {
        if (const auto source = meshes_->image(ref)) {
            if (!source->is_hdr()) {
                image = source->ldr;
            } else { // clamp HDR to 8 bits (a layer texture is a plain colour or data map)
                core::Image ldr;
                ldr.width = source->hdr.width;
                ldr.height = source->hdr.height;
                ldr.rgba.resize(source->hdr.rgba.size());
                for (std::size_t i = 0; i < ldr.rgba.size(); ++i) {
                    float v = std::clamp(source->hdr.rgba[i], 0.0f, 1.0f);
                    v = i % 4 == 3 ? v : core::linear_to_srgb(v);
                    ldr.rgba[i] = static_cast<std::uint8_t>(std::lround(v * 255.0f));
                }
                image = std::move(ldr);
            }
        }
    }
    if (image && !image->empty()) {
        out = terrain::resample_tileable(*image, size);
    } else {
        F3D_LOG_WARN("Terrain layer texture '{}' could not be loaded", ref.path);
    }
    return out.empty() ? nullptr : &out;
}

void RasterEngine::update_layer_textures(rhi::CommandContext& ctx, GpuTerrain& gpu, const terrain::Terrain& t) {
    // Where each slot's texels come from: the layer's own textures, else its material's.
    struct Source {
        core::AssetRef ref;
        std::uint32_t channel = 0;
    };
    struct LayerSources {
        std::array<Source, terrain::kLayerTextureSlots> slots;
        bool directx = false;
    };
    const std::size_t count = std::clamp<std::size_t>(t.layers.size(), 1, terrain::kMaxLayers);
    const std::uint32_t res = std::clamp<std::uint32_t>(std::bit_ceil(std::max(t.texture_resolution, 16u)), 16u, 4096u);
    std::vector<LayerSources> sources(count);
    std::uint64_t key = 1469598103934665603ull;
    const auto mix = [&](std::string_view text) {
        for (const char c : text) {
            key = (key ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
        }
        key = (key ^ 0xffu) * 1099511628211ull;
    };
    mix(std::to_string(res));
    mix(std::to_string(count));
    for (std::size_t i = 0; i < count && i < t.layers.size(); ++i) {
        const terrain::TerrainLayer& layer = t.layers[i];
        LayerSources& src = sources[i];
        src.directx = layer.normal_directx;
        for (std::uint32_t slot = 0; slot < terrain::kLayerTextureSlots; ++slot) {
            src.slots[slot].ref = terrain::layer_texture(layer, static_cast<terrain::LayerTextureSlot>(slot));
        }
        // A packed ARM/ORM map: occlusion in red, roughness in green.
        if (terrain::is_packed_orm(src.slots[2].ref.path)) {
            src.slots[2].channel = 1;
        }
        if (!layer.material.empty() && meshes_) {
            if (const auto material = meshes_->material(layer.material)) {
                auto& slots = src.slots;
                if (slots[0].ref.empty()) {
                    slots[0].ref = material->base_color_texture;
                }
                if (slots[1].ref.empty()) {
                    slots[1].ref = material->normal_texture;
                }
                if (slots[2].ref.empty() && !material->orm_texture.empty()) {
                    slots[2] = {material->orm_texture, 1}; // roughness in green
                }
                if (slots[3].ref.empty()) {
                    if (material->orm_occlusion && !material->orm_texture.empty()) {
                        slots[3] = {material->orm_texture, 0};
                    } else {
                        slots[3] = {material->occlusion_texture, 0};
                    }
                }
            }
        }
        for (const Source& s : src.slots) {
            mix(s.ref.guid.to_string());
            mix(s.ref.path);
            mix(std::to_string(s.channel));
        }
        mix(src.directx ? "dx" : "gl");
    }
    if (gpu.layer_albedo && gpu.layer_key == key) {
        return;
    }
    F3D_ZONE_N("Build terrain layer textures");
    const std::size_t texels = static_cast<std::size_t>(res) * res;
    rhi::TextureDesc desc{"Terrain layer albedo", res, res, rhi::Format::RGBA8Unorm,
                          rhi::BindFlags::ShaderResource | rhi::BindFlags::RenderTarget};
    desc.dimension = rhi::TextureDimension::Tex2DArray;
    desc.array_size = static_cast<std::uint32_t>(count);
    desc.mip_levels = 0;
    desc.generate_mips = true;
    gpu.layer_albedo = device_.create_texture(desc);
    desc.name = "Terrain layer normal";
    gpu.layer_normal = device_.create_texture(desc);
    std::vector<std::uint8_t> albedo(texels * 4);
    std::vector<std::uint8_t> normal(texels * 4);
    for (std::size_t i = 0; i < count; ++i) {
        const LayerSources& src = sources[i];
        const auto* color = layer_image(src.slots[0].ref, res);
        const auto* nmap = layer_image(src.slots[1].ref, res);
        const auto* rough = layer_image(src.slots[2].ref, res);
        const auto* ao = layer_image(src.slots[3].ref, res);
        const auto* height = layer_image(src.slots[4].ref, res);
        std::uint32_t flags = 0;
        flags |= color ? kLayerAlbedo : 0u;
        flags |= nmap ? kLayerNormal : 0u;
        flags |= rough ? kLayerRoughness : 0u;
        flags |= ao ? kLayerAo : 0u;
        flags |= height || color ? kLayerHeight : 0u;
        gpu.layer_flags[i] = flags;
        const std::uint32_t rough_channel = src.slots[2].channel;
        const std::uint32_t ao_channel = src.slots[3].channel;
        for (std::size_t p = 0; p < texels; ++p) {
            const std::size_t o = p * 4;
            if (color) {
                albedo[o + 0] = (*color)[o + 0];
                albedo[o + 1] = (*color)[o + 1];
                albedo[o + 2] = (*color)[o + 2];
                // Height: its own map, else the colour's brightness.
                albedo[o + 3] = height ? (*height)[o]
                                       : static_cast<std::uint8_t>(((*color)[o] * 77u + (*color)[o + 1] * 150u +
                                                                    (*color)[o + 2] * 29u) >> 8);
            } else {
                albedo[o + 0] = albedo[o + 1] = albedo[o + 2] = 255;
                albedo[o + 3] = height ? (*height)[o] : 128;
            }
            // The normal along +u and +v: OpenGL-style maps have green up the image (-v).
            normal[o + 0] = nmap ? (*nmap)[o + 0] : 128;
            normal[o + 1] = !nmap ? 128 : src.directx ? (*nmap)[o + 1] : static_cast<std::uint8_t>(255 - (*nmap)[o + 1]);
            normal[o + 2] = rough ? (*rough)[o + rough_channel] : 255;
            normal[o + 3] = ao ? (*ao)[o + ao_channel] : 255;
        }
        const auto slice = static_cast<std::uint32_t>(i);
        ctx.update_texture(gpu.layer_albedo, 0, 0, res, res, albedo.data(), static_cast<std::size_t>(res) * 4, slice);
        ctx.update_texture(gpu.layer_normal, 0, 0, res, res, normal.data(), static_cast<std::size_t>(res) * 4, slice);
    }
    ctx.generate_mips(gpu.layer_albedo);
    ctx.generate_mips(gpu.layer_normal);
    gpu.layer_key = key;
    gpu.bindings = {};
}

void RasterEngine::draw_terrains(rhi::CommandContext& ctx, TerrainPass pass, const glm::mat4& view_projection) {
    const rhi::Pipeline& pipeline = terrain_pipelines_[static_cast<std::size_t>(pass)];
    if (!world_ || !pipeline) {
        return;
    }
    for (auto& [guid, gpu] : terrains_) {
        if (!gpu.seen || !gpu.bindings[static_cast<std::size_t>(pass)]) {
            continue;
        }
        const entt::entity e = world_->find(guid);
        if (e == entt::null) {
            continue;
        }
        if (pass == TerrainPass::Pick) {
            ObjectConstants object;
            object.world = glm::mat4(1.0f);
            object.normal_matrix = glm::mat4(1.0f);
            object.object_id = glm::uvec4(static_cast<std::uint32_t>(entt::to_integral(e)) + 1u, 0u, 0u, 0u);
            ctx.update_buffer(object_constants_, &object, sizeof(object));
        }
        if (pass == TerrainPass::Lit || pass == TerrainPass::Wireframe) {
            // Each layer's surface: its textures (in the arrays), its material's factors, else its own colour.
            const terrain::Terrain& t = world_->registry().get<terrain::Terrain>(e);
            TerrainLayerConstants layers{};
            const std::size_t count = std::clamp<std::size_t>(t.layers.size(), 1, terrain::kMaxLayers);
            for (std::size_t i = 0; i < count; ++i) {
                const terrain::TerrainLayer layer = i < t.layers.size() ? t.layers[i] : terrain::TerrainLayer{};
                glm::vec3 color = layer.color;
                float roughness = layer.roughness;
                float metallic = 0.0f;
                if (!layer.material.empty() && meshes_) {
                    if (const auto material = meshes_->material(layer.material)) {
                        if (layer.albedo_texture.empty()) { // with the layer's own albedo, its colour stays the tint
                            color = glm::vec3(material->base_color);
                        }
                        roughness = material->roughness;
                        metallic = material->metallic;
                    }
                }
                layers.color[i] = glm::vec4(color, roughness);
                layers.params[i] = glm::vec4(1.0f / std::max(layer.tiling, 0.01f), layer.triplanar ? 1.0f : 0.0f,
                                             metallic, std::max(layer.normal_strength, 0.0f));
                layers.params2[i] = glm::vec4(std::max(layer.height_contrast, 0.0f),
                                              std::clamp(layer.anti_tiling, 0.0f, 1.0f), 0.0f, 0.0f);
                layers.flags[i] = glm::uvec4(gpu.layer_flags[i], 0u, 0u, 0u);
            }
            layers.info = glm::uvec4(static_cast<std::uint32_t>(count), std::max(gpu.splat_maps, 1u), 0u, 0u);
            ctx.update_buffer(terrain_layer_constants_, &layers, sizeof(layers));
        }
        const bool shadow = pass == TerrainPass::Shadow;
        const std::uint32_t first = shadow ? gpu.camera_full + gpu.camera_quarter : 0;
        const std::uint32_t full = shadow ? gpu.shadow_full : gpu.camera_full;
        const std::uint32_t quarter = shadow ? gpu.shadow_quarter : gpu.camera_quarter;
        ctx.set_pipeline(pipeline);
        ctx.set_vertex_buffers({&terrain_grid_vertices_});
        ctx.set_index_buffer(terrain_grid_indices_);
        TerrainConstants c = gpu.constants;
        c.view_projection = view_projection;
        for (int part = 0; part < 2; ++part) {
            const std::uint32_t instances = part == 0 ? full : quarter;
            if (instances == 0) {
                continue;
            }
            c.draw = glm::vec4(static_cast<float>(first + (part == 0 ? 0 : full)), 0.0f, 0.0f, 0.0f);
            ctx.update_buffer(terrain_constants_, &c, sizeof(c));
            ctx.commit(gpu.bindings[static_cast<std::size_t>(pass)]);
            ctx.draw_indexed(part == 0 ? terrain_full_indices_ : terrain_quarter_indices_, rhi::IndexType::UInt32,
                             instances);
            last_mesh_draws_ += pass == TerrainPass::Lit ? 1 : 0;
        }
    }
}

} // namespace f3d::render::raster
