// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/raster_engine.hpp"

#include "f3d/assets/texture.hpp"
#include "f3d/core/debug_draw.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/modeling/editable_mesh.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/sky.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/builtin_textures.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/spline.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace f3d::render::raster {

ViewConstants make_view_constants(const View& view, const GridSettings& grid, const FrameLighting& frame) {
    ViewConstants c{};
    c.view_projection = view.view_projection();
    c.inverse_view_projection = glm::inverse(c.view_projection);
    c.view = view.view;
    c.camera_position = glm::vec4(view.camera_position, view.orthographic ? 1.0f : 0.0f);
    const auto w = static_cast<float>(std::max(view.width, 1u));
    const auto h = static_cast<float>(std::max(view.height, 1u));
    c.viewport = glm::vec4(w, h, 1.0f / w, 1.0f / h);
    // The grid fades out at a distance that grows with the camera's height above it.
    const float height = std::abs(view.camera_position.y);
    // min/max rather than std::clamp: a short far plane (thumbnails) would invert the bounds.
    const float fade_end = std::min(std::max(height * 40.0f, 60.0f), view.far_plane * 0.9f);
    c.grid_params = glm::vec4(fade_end * 0.4f, fade_end, grid.minor_spacing, grid.major_spacing);
    c.frame_params = glm::vec4(frame.exposure_scale, static_cast<float>(kSpecularMips),
                               frame.gpu_exposure ? 1.0f : 0.0f, static_cast<float>(frame.view_mode));

    std::uint32_t directional = 0;
    for (const DirectionalLight& d : frame.directional) {
        if (directional == kMaxDirectionalLights) {
            break;
        }
        c.directional_direction[directional] = glm::vec4(glm::normalize(d.direction), 0.0f);
        c.directional_color[directional] = glm::vec4(d.color, 0.0f);
        ++directional;
    }
    const bool shadowed = frame.shadow_light >= 0 && static_cast<std::uint32_t>(frame.shadow_light) < directional;
    c.light_counts = glm::uvec4(frame.local_lights, directional,
                                shadowed ? static_cast<std::uint32_t>(frame.shadow_light) + 1u : 0u, 0u);

    c.inverse_projection = glm::inverse(view.projection);
    const glm::uvec2 tile = cluster_tile_size(view);
    const ClusterDepth depth = cluster_depth(view);
    c.cluster_params = glm::vec4(static_cast<float>(tile.x), static_cast<float>(tile.y), depth.scale, depth.bias);
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        const auto k = static_cast<glm::length_t>(i);
        c.shadow_matrices[i] = frame.cascades.view_projection[i];
        c.shadow_splits[k] = frame.cascades.split_far[i];
        c.shadow_texel[k] = frame.cascades.texel_size[i];
    }
    return c;
}

RasterEngine::RasterEngine(rhi::Device& device, rhi::ShaderLibrary& shaders)
    : device_(device), shaders_(shaders), fog_(device, shaders), bloom_(device, shaders), sky_(device, shaders),
      transients_(device) {
    shadow_pass_constants_ = device_.create_buffer(
        {"Shadow pass constants", sizeof(glm::mat4), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    view_constants_ = device_.create_buffer(
        {"View constants", sizeof(ViewConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    object_constants_ = device_.create_buffer(
        {"Object constants", sizeof(ObjectConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    material_constants_ = device_.create_buffer(
        {"Material constants", sizeof(MaterialConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    const auto storage = rhi::BindFlags::ShaderResource | rhi::BindFlags::UnorderedAccess;
    cluster_counts_ = device_.create_buffer(
        {"Cluster light counts", kClusterCount * sizeof(std::uint32_t), storage, rhi::Usage::Default, 4});
    cluster_indices_ =
        device_.create_buffer({"Cluster light indices", kClusterCount * kMaxLightsPerCluster * sizeof(std::uint32_t),
                               storage, rhi::Usage::Default, 4});
    create_default_textures();

    resolve_constants_ = device_.create_buffer(
        {"Resolve constants", sizeof(ResolveConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    exposure_constants_ = device_.create_buffer(
        {"Exposure constants", sizeof(ExposureConstants), rhi::BindFlags::UniformBuffer, rhi::Usage::Dynamic});
    const glm::vec4 initial(1.0f, 0.0f, 0.0f, 0.0f);
    exposure_state_ = device_.create_buffer(
        {"Exposure state", sizeof(glm::vec4), storage, rhi::Usage::Default, sizeof(glm::vec4)}, &initial);
    const std::array<std::uint32_t, 256> zeros{};
    histogram_ = device_.create_buffer(
        {"Exposure histogram", sizeof(zeros), storage, rhi::Usage::Default, sizeof(std::uint32_t)}, zeros.data());
}

void RasterEngine::create_default_textures() {
    const std::uint32_t white = 0xFFFFFFFFu;
    white_ =
        device_.create_texture({"White", 1, 1, rhi::Format::RGBA8Unorm, rhi::BindFlags::ShaderResource}, &white, 4);
    const std::uint32_t flat = 0xFFFF8080u; // (0.5, 0.5, 1): +Z in tangent space
    flat_normal_ = device_.create_texture(
        {"Flat normal", 1, 1, rhi::Format::RGBA8Unorm, rhi::BindFlags::ShaderResource}, &flat, 4);
}

void RasterEngine::create_pipelines() {
    F3D_ZONE;
    shader_generation_ = shaders_.generation();

    rhi::GraphicsPipelineDesc grid;
    grid.name = "Grid";
    grid.vs = shaders_.load("fullscreen.vs");
    grid.ps = shaders_.load("grid.ps");
    grid.cull = rhi::CullMode::None;
    grid.blend = rhi::BlendMode::AlphaBlend;
    // Tested against opaque geometry (drawn first) but not written, so lines and
    // later transparent passes on the ground plane don't z-fight with it.
    grid.depth_test = true;
    grid.depth_write = false;
    grid.color_formats = {display_format()}; // drawn after tonemapping
    grid.depth_format = depth_format();
    if (grid.vs && grid.ps) {
        grid_pipeline_ = device_.create_graphics_pipeline(grid);
        grid_bindings_ = grid_pipeline_.create_bindings();
        grid_bindings_.set("ViewConstants", view_constants_);
    }

    const std::vector<rhi::VertexAttribute> mesh_layout = {
        {0, 0, rhi::Format::RGB32Float, offsetof(assets::MeshVertex, position)},
        {1, 0, rhi::Format::RGB32Float, offsetof(assets::MeshVertex, normal)},
        {2, 0, rhi::Format::RGBA8Unorm, offsetof(assets::MeshVertex, color)},
        {3, 1, rhi::Format::RG32Float, 0}};

    rhi::GraphicsPipelineDesc mesh;
    mesh.vs = shaders_.load("mesh.vs");
    mesh.ps = shaders_.load("mesh.ps");
    mesh.vertex_layout = mesh_layout;
    for (const char* texture : {"g_BaseColor", "g_Normal", "g_Orm", "g_Occlusion", "g_Emissive"}) {
        mesh.samplers.push_back({texture, rhi::Filter::Linear, rhi::AddressMode::Wrap});
    }
    mesh.samplers.push_back({"g_EnvSpecular", rhi::Filter::Linear, rhi::AddressMode::Clamp});
    mesh.samplers.push_back({"g_ShadowMap", rhi::Filter::Linear, rhi::AddressMode::Clamp, rhi::CompareFunc::LessEqual});
    mesh.depth_test = true;
    mesh.depth_write = true;
    mesh.color_formats = {color_format()};
    mesh.depth_format = depth_format();
    mesh_bindings_.clear(); // made for the old pipelines
    if (mesh.vs && mesh.ps) {
        const std::array<std::pair<const char*, rhi::CullMode>, 3> variants = {
            {{"Mesh (PBR)", rhi::CullMode::Back},
             {"Mesh (PBR, mirrored)", rhi::CullMode::Front}, // negative-determinant transforms
             {"Mesh (PBR, double-sided)", rhi::CullMode::None}}};
        for (std::size_t i = 0; i < variants.size(); ++i) {
            mesh.name = variants[i].first;
            mesh.cull = variants[i].second;
            mesh_pipelines_[i] = device_.create_graphics_pipeline(mesh);
        }
        mesh.name = "Mesh (wireframe)";
        mesh.cull = rhi::CullMode::None;
        mesh.wireframe = true;
        mesh_pipelines_[static_cast<std::size_t>(Variant::Wireframe)] = device_.create_graphics_pipeline(mesh);
    }

    const auto compute = [&](const char* name, const char* shader) {
        rhi::ComputePipelineDesc desc;
        desc.name = name;
        desc.cs = shaders_.load(shader);
        return desc.cs ? device_.create_compute_pipeline(desc) : rhi::Pipeline{};
    };
    histogram_pipeline_ = compute("Exposure histogram", "exposure_histogram.cs");
    adapt_pipeline_ = compute("Exposure adaptation", "exposure_adapt.cs");
    histogram_bindings_ = {};
    histogram_source_ = {};
    adapt_bindings_ = {};
    if (adapt_pipeline_) {
        adapt_bindings_ = adapt_pipeline_.create_bindings();
        adapt_bindings_.set("ExposureConstants", exposure_constants_);
        adapt_bindings_.set("g_ExposureState", exposure_state_);
        adapt_bindings_.set("g_Histogram", histogram_);
    }

    rhi::GraphicsPipelineDesc shadow;
    shadow.name = "Shadow casters";
    shadow.vs = shaders_.load("shadow.vs");
    shadow.vertex_layout = mesh_layout;
    shadow.cull = rhi::CullMode::None; // open meshes cast from both sides
    shadow.depth_test = true;
    shadow.depth_write = true;
    shadow.slope_scaled_depth_bias = 1.5f;
    shadow.depth_clip = false; // casters in front of the cascade are clamped onto its near plane
    shadow.depth_format = rhi::Format::D32Float;
    shadow_pipeline_ = shadow.vs ? device_.create_graphics_pipeline(shadow) : rhi::Pipeline{};
    shadow_bindings_ = {};
    if (shadow_pipeline_) {
        shadow_bindings_ = shadow_pipeline_.create_bindings();
        shadow_bindings_.set("ObjectConstants", object_constants_);
        shadow_bindings_.set("ShadowPassConstants", shadow_pass_constants_);
    }

    create_terrain_pipelines({});

    rhi::ComputePipelineDesc cluster;
    cluster.name = "Light clustering";
    cluster.cs = shaders_.load("light_cluster.cs");
    if (cluster.cs) {
        cluster_pipeline_ = device_.create_compute_pipeline(cluster);
        cluster_bindings_ = {}; // bound to the light buffer on first use
    }

    rhi::GraphicsPipelineDesc pick;
    pick.name = "Pick (ID buffer)";
    pick.vs = shaders_.load("mesh.vs");
    pick.ps = shaders_.load("pick.ps");
    pick.vertex_layout = mesh_layout;
    pick.cull = rhi::CullMode::None;
    pick.depth_test = true;
    pick.depth_write = true;
    pick.color_formats = {rhi::Format::R32Uint};
    pick.depth_format = depth_format();
    if (pick.vs && pick.ps) {
        pick_pipeline_ = device_.create_graphics_pipeline(pick);
        pick_bindings_ = pick_pipeline_.create_bindings();
        pick_bindings_.set("ViewConstants", view_constants_);
        pick_bindings_.set("ObjectConstants", object_constants_);
    }

    rhi::GraphicsPipelineDesc lines;
    lines.name = "Debug lines";
    lines.vs = shaders_.load("debug_lines.vs");
    lines.ps = shaders_.load("debug_lines.ps");
    lines.vertex_layout = {{0, 0, rhi::Format::RGB32Float, 0}, {1, 0, rhi::Format::RGBA8Unorm, 12}};
    lines.topology = rhi::PrimitiveTopology::LineList;
    lines.cull = rhi::CullMode::None;
    lines.blend = rhi::BlendMode::AlphaBlend;
    lines.depth_test = true;
    lines.depth_write = false;
    lines.color_formats = {display_format()};
    lines.depth_format = depth_format();
    if (lines.vs && lines.ps) {
        line_pipeline_ = device_.create_graphics_pipeline(lines);
        line_bindings_ = line_pipeline_.create_bindings();
        line_bindings_.set("ViewConstants", view_constants_);
    }

    rhi::GraphicsPipelineDesc resolve;
    resolve.name = "Viewport resolve";
    resolve.vs = shaders_.load("fullscreen.vs");
    resolve.ps = shaders_.load("resolve.ps");
    resolve.cull = rhi::CullMode::None;
    resolve.depth_test = false;
    resolve.depth_write = false;
    resolve.color_formats = {display_format()};
    resolve.samplers.push_back({"g_SceneColor", rhi::Filter::Linear, rhi::AddressMode::Clamp});
    resolve.samplers.push_back({"g_Bloom", rhi::Filter::Linear, rhi::AddressMode::Clamp});
    if (resolve.vs && resolve.ps) {
        resolve_pipeline_ = device_.create_graphics_pipeline(resolve);
        resolve_source_ = {};
    }

    rhi::GraphicsPipelineDesc depth_upscale;
    depth_upscale.name = "Depth upscale";
    depth_upscale.vs = shaders_.load("fullscreen.vs");
    depth_upscale.ps = shaders_.load("depth_upscale.ps");
    depth_upscale.cull = rhi::CullMode::None;
    depth_upscale.depth_test = true;
    depth_upscale.depth_write = true;
    depth_upscale.depth_func = rhi::CompareFunc::Always;
    depth_upscale.depth_format = depth_format();
    depth_upscale_pipeline_ = depth_upscale.vs && depth_upscale.ps ? device_.create_graphics_pipeline(depth_upscale)
                                                                   : rhi::Pipeline{};
    depth_upscale_source_ = {};

    fog_.create_pipelines();
    bloom_.create_pipelines();
    create_water_pipelines();
}

void RasterEngine::set_scene(const RenderScene& scene) noexcept {
    world_ = scene.world;
    meshes_ = scene.meshes;
}

void RasterEngine::clear_texture_cache() {
    path_textures_.clear();
    mesh_bindings_.clear();
    layer_images_.clear(); // terrain layer arrays rebuild from the files again
    for (auto& [guid, gpu] : terrains_) {
        gpu.layer_key = 0;
    }
}

// --- Meshes & materials ---------------------------------------------------------------------

const RasterEngine::GpuMesh* RasterEngine::gpu_mesh(const core::AssetRef& ref) {
    const auto source = meshes_->get(ref);
    if (!source || source->indices.empty()) {
        return nullptr;
    }
    GpuMesh& gpu = gpu_meshes_[ref.path];
    if (gpu.source.lock() == source && gpu.index_count > 0) {
        return &gpu;
    }
    F3D_ZONE_N("Upload mesh");
    gpu.source = source;
    gpu.index_count = 0;
    gpu.submeshes = assets::submeshes_of(*source);
    gpu.vertices =
        device_.create_buffer({source->name + " vertices", source->vertices.size() * sizeof(assets::MeshVertex),
                               rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
                              source->vertices.data());
    gpu.indices = device_.create_buffer({source->name + " indices", source->indices.size() * sizeof(std::uint32_t),
                                         rhi::BindFlags::IndexBuffer, rhi::Usage::Immutable},
                                        source->indices.data());
    std::vector<glm::vec2> uvs = source->uvs;
    uvs.resize(source->vertices.size(), glm::vec2(0.0f)); // zeros when the mesh has none
    gpu.uvs = device_.create_buffer(
        {source->name + " uvs", uvs.size() * sizeof(glm::vec2), rhi::BindFlags::VertexBuffer, rhi::Usage::Immutable},
        uvs.data());
    if (!gpu.vertices || !gpu.indices || !gpu.uvs) {
        return nullptr;
    }
    gpu.index_count = static_cast<std::uint32_t>(source->indices.size());
    return &gpu;
}

float RasterEngine::screen_radius(const core::Bounds& local_bounds, const glm::mat4& world) const noexcept {
    if (local_bounds.empty()) {
        return 0.0f;
    }
    const core::Bounds b = local_bounds.transformed(world);
    const float radius = glm::length(b.extent()) * 0.5f;
    const float half_height = static_cast<float>(view_.height) * 0.5f;
    const float scale = view_.projection[1][1]; // 1 / tan(fov / 2), or 2 / ortho height
    if (view_.orthographic) {
        return radius * scale * half_height;
    }
    const float distance = std::max(glm::distance(b.center(), view_.camera_position) - radius, view_.near_plane);
    return radius * scale / distance * half_height;
}

bool RasterEngine::resolve(const scene::MeshRenderer& renderer, const glm::mat4& world, DrawItem& item) {
    if (!renderer.mesh.guid.is_null() && residency_) {
        // A project asset: the LOD that is resident for its size on screen.
        const float px = screen_radius(residency_->mesh_bounds(renderer.mesh), world);
        const GpuMeshLod* lod = residency_->mesh(renderer.mesh, px);
        if (!lod) {
            return false; // streaming in
        }
        item = {&lod->vertices,
                &lod->uvs,
                &lod->indices,
                lod->index_count,
                &lod->submeshes,
                px,
                residency_->mesh_bounds(renderer.mesh)};
        return true;
    }
    const GpuMesh* mesh = gpu_mesh(renderer.mesh);
    if (!mesh) {
        return false;
    }
    const auto source = mesh->source.lock();
    const float px = screen_radius(source ? source->bounds : core::Bounds{}, world);
    item = {&mesh->vertices,
            &mesh->uvs,
            &mesh->indices,
            mesh->index_count,
            &mesh->submeshes,
            px,
            source ? source->bounds : core::Bounds{}};
    return true;
}

core::Bounds RasterEngine::mesh_bounds(const core::AssetRef& ref) {
    if (!ref.guid.is_null() && residency_) {
        return residency_->mesh_bounds(ref);
    }
    const GpuMesh* mesh = gpu_mesh(ref);
    const auto source = mesh ? mesh->source.lock() : nullptr;
    return source ? source->bounds : core::Bounds{};
}

void RasterEngine::collect_draws(DrawPass pass, std::vector<MeshDraw>& out) {
    out.clear();
    scatter_meshes_.clear();
    world_->update_transforms();
    const auto& registry = world_->registry();
    const std::array<glm::vec4, 6> frustum = terrain::frustum_planes(view_.view_projection());
    const auto shown = [&](entt::entity e) {
        return pass == DrawPass::Pick ? scene::is_pickable(*world_, e) : scene::is_visible(*world_, e);
    };
    for (const entt::entity e : world_->depth_first()) {
        if (const auto* renderer = registry.try_get<scene::MeshRenderer>(e);
            renderer && renderer->visible && shown(e)) {
            out.push_back({e, renderer, registry.get<scene::Transform>(e).world});
        }
        const auto* scatter = registry.try_get<world::Scatter>(e);
        if (!scatter || !scatter->visible || (pass == DrawPass::Shadow && !scatter->cast_shadows) || !shown(e)) {
            continue;
        }
        const std::vector<world::ScatterInstance>& instances = world::scatter_instances(*world_, e);
        if (instances.empty()) {
            continue;
        }
        // Per mesh: a renderer to draw it with, and its local bounding sphere.
        const std::size_t first = scatter_meshes_.size();
        std::vector<glm::vec4> spheres;
        for (const core::AssetRef& mesh : scatter->meshes) {
            scatter_meshes_.push_back(scene::MeshRenderer{mesh, true, {}, {}});
            const core::Bounds b = mesh_bounds(mesh);
            spheres.push_back(b.empty() ? glm::vec4(0.0f, 0.0f, 0.0f, -1.0f)
                                        : glm::vec4(b.center(), glm::length(b.extent()) * 0.5f));
        }
        const float range = std::max(scatter->draw_distance, 0.0f);
        for (const world::ScatterInstance& instance : instances) {
            const glm::vec4& sphere = spheres[instance.mesh];
            if (sphere.w < 0.0f) {
                continue; // not loaded yet
            }
            const glm::vec3 centre = glm::vec3(instance.world * glm::vec4(glm::vec3(sphere), 1.0f));
            const float radius = sphere.w * glm::length(glm::vec3(instance.world[0]));
            if (glm::distance(centre, view_.camera_position) - radius > range) {
                continue;
            }
            if (pass != DrawPass::Shadow &&
                std::any_of(frustum.begin(), frustum.end(), [&](const glm::vec4& p) {
                    return glm::dot(glm::vec3(p), centre) + p.w < -radius;
                })) {
                continue;
            }
            out.push_back({e, &scatter_meshes_[first + instance.mesh], instance.world});
        }
    }
    collect_spline_draws(pass, out);
}

void RasterEngine::collect_spline_draws(DrawPass pass, std::vector<MeshDraw>& out) {
    if (!meshes_) {
        return;
    }
    const auto& registry = world_->registry();
    for (const entt::entity e : world_->depth_first()) {
        const auto* spline = registry.try_get<world::Spline>(e);
        const bool shown = pass == DrawPass::Pick ? scene::is_pickable(*world_, e) : scene::is_visible(*world_, e);
        if (!spline || !spline->visible || !shown) {
            continue;
        }
        const world::SplineGeometry& geometry = world::spline_geometry(*world_, e);
        if (geometry.surface && pass != DrawPass::Shadow) {
            // The ribbon is registered as a mesh (again when it changes) with a material of
            // the built-in surface's textures.
            const std::string path = "spline:" + world_->guid(e).to_string();
            if (spline_meshes_[path] != geometry.revision) {
                meshes_->add(path, *geometry.surface);
                spline_meshes_[path] = geometry.revision;
            }
            const std::string material = "spline-surface:" + spline->surface;
            if (spline_meshes_.emplace(material, 1).second) { // registered once
                assets::Material m;
                m.name = spline->surface;
                m.roughness = 1.0f;
                m.base_color_texture = {core::Guid{}, terrain::builtin_texture_path(spline->surface,
                                                                                    terrain::LayerTextureSlot::Albedo)};
                m.normal_texture = {core::Guid{}, terrain::builtin_texture_path(spline->surface,
                                                                                terrain::LayerTextureSlot::Normal)};
                m.orm_texture = {core::Guid{}, terrain::builtin_texture_path(spline->surface,
                                                                             terrain::LayerTextureSlot::Roughness)};
                meshes_->add_material(material, std::move(m));
            }
            scatter_meshes_.push_back(
                scene::MeshRenderer{{core::Guid{}, path}, true, {}, {core::AssetRef{core::Guid{}, material}}});
            out.push_back({e, &scatter_meshes_.back(), glm::mat4(1.0f)});
        }
        for (const world::SplineProp& prop : geometry.props) {
            scatter_meshes_.push_back(scene::MeshRenderer{prop.mesh, true, {}, {}});
            out.push_back({e, &scatter_meshes_.back(), prop.world});
        }
    }
}

rhi::Texture RasterEngine::slot_texture(const core::AssetRef& ref, float screen_px) {
    if (ref.empty()) {
        return {};
    }
    if (!ref.guid.is_null()) {
        if (residency_) {
            if (const GpuTexture* texture = residency_->texture(ref, screen_px)) {
                return texture->texture;
            }
        }
        return {};
    }
    // A model loaded by path: decode its image once (RGBA8 with mips, no compression).
    if (const auto it = path_textures_.find(ref.path); it != path_textures_.end()) {
        return it->second;
    }
    rhi::Texture& texture = path_textures_[ref.path];
    if (auto builtin = terrain::builtin_texture(ref.path)) { // spline surfaces use the terrain's built-ins
        assets::TextureSettings settings;
        settings.kind = ref.path.ends_with("/albedo") ? assets::TextureKind::Color
                        : ref.path.ends_with("/normal") ? assets::TextureKind::Normal
                                                        : assets::TextureKind::Linear;
        settings.compression = "none";
        texture = upload_texture(device_, assets::cook_texture(assets::SourceImage{std::move(*builtin), {}}, settings),
                                 0, ref.path);
        return texture;
    }
    if (!meshes_) {
        return texture;
    }
    F3D_ZONE_N("Load model image");
    assets::TextureKind kind = assets::TextureKind::Color;
    if (const auto image = meshes_->image(ref, &kind)) {
        assets::TextureSettings settings;
        settings.kind = image->is_hdr() ? assets::TextureKind::Hdr : kind;
        settings.compression = "none";
        texture = upload_texture(device_, assets::cook_texture(*image, settings), 0, ref.path);
    }
    return texture;
}

RasterEngine::DrawMaterial RasterEngine::draw_material(const assets::Material* material,
                                                       const core::AssetRef& texture_override, float screen_px) {
    static const assets::Material kDefault;
    const assets::Material& m = material ? *material : kDefault;
    DrawMaterial out;
    MaterialConstants& c = out.constants;
    c.base_color = m.base_color;
    c.emissive = glm::vec4(m.emissive * m.emissive_strength, 0.0f);
    c.metallic = m.metallic;
    c.roughness = m.roughness;
    c.normal_scale = m.normal_scale;
    c.occlusion_strength = m.occlusion_strength;
    c.alpha_cutoff = m.alpha_cutoff;
    out.double_sided = m.double_sided;

    const auto pick = [&](const core::AssetRef& ref, const rhi::Texture& fallback) {
        rhi::Texture t = slot_texture(ref, screen_px);
        return std::make_pair(t ? t : fallback, static_cast<bool>(t));
    };
    const auto [base, has_base] = pick(texture_override.empty() ? m.base_color_texture : texture_override, white_);
    const auto [normal, has_normal] = pick(m.normal_texture, flat_normal_);
    const auto [orm, has_orm] = pick(m.orm_texture, white_);
    const auto [occlusion, has_occlusion] = pick(m.occlusion_texture, white_);
    const auto [emissive, has_emissive] = pick(m.emissive_texture, white_);
    (void)has_base;
    (void)has_emissive;
    out.textures = {base, normal, orm, occlusion, emissive};
    c.flags = (has_normal ? kMaterialNormalMap : 0u) | (has_orm && m.orm_occlusion ? kMaterialOrmOcclusion : 0u) |
              (m.alpha_mode == assets::AlphaMode::Mask ? kMaterialAlphaMask : 0u) |
              (has_occlusion ? kMaterialOcclusionMap : 0u);
    return out;
}

const rhi::Bindings& RasterEngine::bindings_for(const TextureSet& textures, const rhi::Texture& environment,
                                                Variant variant) {
    std::array<void*, 6> key{};
    for (std::size_t i = 0; i < textures.size(); ++i) {
        key[i] = textures[i].native();
    }
    key[5] = environment.native();
    CachedBindings& entry = mesh_bindings_[{key, variant}];
    entry.last_frame = draw_frame_;
    if (!entry.bindings) {
        entry.bindings = mesh_pipelines_[static_cast<std::size_t>(variant)].create_bindings();
        rhi::Bindings& b = entry.bindings;
        b.set("ViewConstants", view_constants_);
        b.set("ObjectConstants", object_constants_);
        b.set("MaterialConstants", material_constants_);
        b.set("g_Lights", lights_);
        b.set("g_ClusterCounts", cluster_counts_);
        b.set("g_ClusterIndices", cluster_indices_);
        b.set("g_EnvSH", sky_.sh_buffer());
        b.set("g_EnvSpecular", environment);
        b.set("g_ShadowMap", shadow_map_);
        b.set("g_Exposure", exposure_state_);
        const std::array<const char*, 5> names = {"g_BaseColor", "g_Normal", "g_Orm", "g_Occlusion", "g_Emissive"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            b.set(names[i], textures[i]);
        }
    }
    return entry.bindings;
}

RasterEngine::Environment RasterEngine::environment_at(const glm::vec3& centre) const {
    Environment out{0, &sky_.environment()};
    if (capturing_probe_) {
        return out; // probes see the sky's environment, not each other
    }
    float best = std::numeric_limits<float>::max();
    for (const auto& [key, probe] : probes_) {
        if (!probe.baked || !probe.seen) {
            continue;
        }
        const float d = glm::distance(centre, probe.position);
        if (d <= probe.radius && d < best) {
            best = d;
            out = {probe.slot, &probe.specular};
        }
    }
    return out;
}

void RasterEngine::draw_meshes(rhi::CommandContext& ctx) {
    last_mesh_draws_ = 0;
    if (!world_ || !meshes_ || !mesh_pipelines_[0]) {
        return;
    }
    F3D_ZONE;
    ++draw_frame_;
    world::prune_scatter_cache(*world_);
    const rhi::Pipeline* bound = nullptr;
    // Draw in scene order so the result doesn't depend on the ECS storage order.
    collect_draws(DrawPass::Lit, draws_);
    // The instances of a scatter mesh share their materials: resolved once per run of them.
    const scene::MeshRenderer* materials_of = nullptr;
    std::vector<DrawMaterial> materials;
    for (const MeshDraw& d : draws_) {
        const entt::entity e = d.entity;
        const scene::MeshRenderer* renderer = d.renderer;
        const glm::mat4& world = d.world;
        DrawItem item;
        if (!resolve(*renderer, world, item)) {
            continue;
        }
        if (renderer != materials_of || materials.size() != item.submeshes->size()) {
            materials_of = renderer;
            materials.clear();
            for (std::size_t slot = 0; slot < item.submeshes->size(); ++slot) {
                const core::AssetRef ref = assets::MeshLibrary::slot_material(*renderer, slot, (*item.submeshes)[slot]);
                const auto material = ref.empty() ? nullptr : meshes_->material(ref);
                materials.push_back(draw_material(material.get(), renderer->texture, item.screen_px));
            }
        }
        const Environment env =
            environment_at(item.bounds.empty() ? glm::vec3(world[3]) : item.bounds.transformed(world).center());
        ObjectConstants constants;
        constants.world = world;
        constants.normal_matrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world))));
        constants.object_id = glm::uvec4(static_cast<std::uint32_t>(entt::to_integral(e)) + 1u, env.slot, 0u, 0u);
        ctx.update_buffer(object_constants_, &constants, sizeof(constants));
        // Probe captures render mirrored cube faces, which swaps front and back faces.
        const bool mirrored = (glm::determinant(glm::mat3(world)) < 0.0f) != capturing_probe_;

        ctx.set_vertex_buffers({item.vertices, item.uvs});
        ctx.set_index_buffer(*item.indices);
        for (std::size_t slot = 0; slot < item.submeshes->size(); ++slot) {
            const assets::SubMesh& submesh = (*item.submeshes)[slot];
            const DrawMaterial& draw = materials[slot];
            ctx.update_buffer(material_constants_, &draw.constants, sizeof(draw.constants));
            const Variant variant =
                settings_.mode == ViewMode::Wireframe
                    ? Variant::Wireframe
                    : (draw.double_sided ? Variant::DoubleSided : (mirrored ? Variant::Mirrored : Variant::Normal));
            const rhi::Pipeline& pipeline = mesh_pipelines_[static_cast<std::size_t>(variant)];
            if (bound != &pipeline) {
                ctx.set_pipeline(pipeline);
                bound = &pipeline;
            }
            // Dynamic constants are re-read at commit time, so commit for every draw.
            ctx.commit(bindings_for(draw.textures, *env.specular, variant));
            ctx.draw_indexed(submesh.index_count, rhi::IndexType::UInt32, 1, submesh.first_index);
            ++last_mesh_draws_;
        }
    }
    // Bindings of textures no longer drawn (streamed out or replaced) go; the GPU
    // keeps what frames in flight still use.
    std::erase_if(mesh_bindings_, [&](const auto& entry) { return entry.second.last_frame + 2 < draw_frame_; });
}

// --- Lights ------------------------------------------------------------------------------------

void RasterEngine::upload_lights(rhi::CommandContext& ctx, const SceneLights& lights) {
    const auto count = static_cast<std::uint32_t>(lights.local.size());
    if (!lights_ || count > light_capacity_) {
        light_capacity_ = std::max<std::uint32_t>(std::max(count, 1u) * 2, 64);
        lights_ = device_.create_buffer({"Lights", light_capacity_ * sizeof(GpuLight), rhi::BindFlags::ShaderResource,
                                         rhi::Usage::Default, sizeof(GpuLight)});
        mesh_bindings_.clear(); // they point at the old buffer
        cluster_bindings_ = {};
    }
    if (count > 0) {
        ctx.update_buffer(lights_, lights.local.data(), count * sizeof(GpuLight));
    }
    last_local_lights_ = count;
}

void RasterEngine::cluster_lights(rhi::CommandContext& ctx) {
    if (!cluster_pipeline_ || !lights_) {
        return;
    }
    if (!cluster_bindings_) {
        cluster_bindings_ = cluster_pipeline_.create_bindings();
        cluster_bindings_.set("ViewConstants", view_constants_);
        cluster_bindings_.set("g_Lights", lights_);
        cluster_bindings_.set("g_ClusterCounts", cluster_counts_);
        cluster_bindings_.set("g_ClusterIndices", cluster_indices_);
    }
    ctx.set_pipeline(cluster_pipeline_);
    ctx.commit(cluster_bindings_);
    ctx.dispatch((kClusterCount + 63) / 64);
}

// --- Frame -----------------------------------------------------------------------------------

void RasterEngine::draw_lines(rhi::CommandContext& ctx) {
    const auto vertices = core::debug::take_lines();
    last_line_vertices_ = static_cast<std::uint32_t>(vertices.size());
    if (vertices.empty() || !line_pipeline_) {
        return;
    }
    if (vertices.size() > line_capacity_) {
        line_capacity_ = std::max<std::uint64_t>(vertices.size() * 2, 4096);
        line_vertices_ = device_.create_buffer({"Debug line vertices", line_capacity_ * sizeof(core::debug::LineVertex),
                                                rhi::BindFlags::VertexBuffer, rhi::Usage::Dynamic});
    }
    // Dynamic buffers are rewritten whole; pad with the uploaded vertices only.
    ctx.update_buffer(line_vertices_, vertices.data(), vertices.size() * sizeof(core::debug::LineVertex));
    ctx.set_pipeline(line_pipeline_);
    ctx.commit(line_bindings_);
    ctx.set_vertex_buffer(line_vertices_);
    ctx.draw(static_cast<std::uint32_t>(vertices.size()));
}

// --- Sky, shadows and probes ----------------------------------------------------------------

FrameLighting RasterEngine::make_frame_lighting(const View& view, const SceneLights& lights) {
    F3D_ZONE;
    FrameLighting frame;
    frame.local_lights = static_cast<std::uint32_t>(lights.local.size());
    const entt::entity sky_entity = world_ ? scene::active_sky(*world_) : entt::entity{entt::null};
    if (sky_entity != entt::null) {
        const scene::Sky& sky = world_->registry().get<scene::Sky>(sky_entity);
        // The CPU sky integrals are cached: recomputed when the Sky or the camera
        // height (in 10 m steps) changes.
        const float height = std::floor(std::max(view.camera_position.y, 0.0f) / 10.0f) * 10.0f;
        if (!cached_sky_valid_ || !(cached_sky_ == sky) || cached_sky_height_ != height) {
            const scene::Celestial celestial = scene::celestial(sky);
            const AtmosphereParams atmosphere = atmosphere_params(sky);
            sky_lighting_ = sky_lighting(sky, celestial, atmosphere, height);
            sky_constants_ = make_sky_constants(atmosphere, sky, sky_lighting_, height);
            cached_sky_ = sky;
            cached_sky_height_ = height;
            cached_sky_valid_ = true;
        }
        const auto lit = [](const glm::vec3& c) { return std::max({c.x, c.y, c.z}) > 0.0f; };
        if (lit(sky_lighting_.sun_illuminance)) {
            frame.directional.push_back({sky_lighting_.sun_direction, sky_lighting_.sun_illuminance,
                                         sky.shadows && !sky_lighting_.shadow_from_moon});
        }
        if (lit(sky_lighting_.moon_illuminance)) {
            frame.directional.push_back({sky_lighting_.moon_direction, sky_lighting_.moon_illuminance,
                                         sky.shadows && sky_lighting_.shadow_from_moon});
        }
        frame.exposure = sky_lighting_.exposure;
    } else {
        cached_sky_valid_ = false;
        sky_constants_ = make_hemisphere_constants(lighting_.sky_color * lighting_.ambient,
                                                   lighting_.ground_color * lighting_.ambient);
        if (lights.directional.empty()) {
            frame.directional.push_back({lighting_.sun_direction, lighting_.sun_color, true});
        }
    }
    for (const DirectionalLight& d : lights.directional) {
        if (frame.directional.size() < kMaxDirectionalLights) {
            frame.directional.push_back(d);
        }
    }
    // Shadows need something to cast them: an empty scene skips the pass (and its memory).
    const bool casters = world_ && (!world_->registry().view<scene::MeshRenderer>().empty() ||
                                    !world_->registry().view<world::Scatter>().empty() ||
                                    !world_->registry().view<terrain::Terrain>().empty());
    if (shadow_settings_.enabled && casters) {
        for (std::size_t i = 0; i < frame.directional.size(); ++i) {
            if (frame.directional[i].cast_shadows) {
                frame.shadow_light = static_cast<std::int32_t>(i);
                frame.cascades = compute_cascades(view, frame.directional[i].direction, shadow_settings_);
                break;
            }
        }
    }
    return frame;
}

void RasterEngine::ensure_shadow_map(bool needed) {
    // Without shadows a 1x1 placeholder is bound, so the full map is only allocated once used.
    const std::uint32_t resolution = needed ? std::max(shadow_settings_.resolution, 16u) : 1u;
    if (shadow_map_ && (shadow_map_.width() == resolution || !needed)) {
        return;
    }
    rhi::TextureDesc desc;
    desc.name = "Shadow map";
    desc.width = desc.height = resolution;
    desc.format = rhi::Format::D32Float;
    desc.bind = rhi::BindFlags::DepthStencil | rhi::BindFlags::ShaderResource;
    desc.dimension = rhi::TextureDimension::Tex2DArray;
    desc.array_size = kShadowCascades;
    shadow_map_ = device_.create_texture(desc);
    mesh_bindings_.clear(); // they point at the old shadow map
}

void RasterEngine::draw_shadows(rhi::CommandContext& ctx) {
    F3D_ZONE;
    if (!shadow_pipeline_ || !world_ || !meshes_ || frame_.shadow_light < 0) {
        return;
    }
    // Casters with their world bounds, gathered once for the four cascades.
    struct Caster {
        const scene::MeshRenderer* renderer;
        glm::mat4 world;
        DrawItem item;
        glm::vec3 centre;
        float radius;
    };
    std::vector<Caster> casters;
    collect_draws(DrawPass::Shadow, draws_);
    for (const MeshDraw& d : draws_) {
        Caster c{d.renderer, d.world, {}, glm::vec3(0.0f), 0.0f};
        if (!resolve(*d.renderer, c.world, c.item)) {
            continue;
        }
        const core::Bounds b = c.item.bounds.empty() ? core::Bounds{} : c.item.bounds.transformed(c.world);
        c.centre = b.empty() ? glm::vec3(c.world[3]) : b.center();
        c.radius = b.empty() ? 0.0f : glm::length(b.extent()) * 0.5f;
        casters.push_back(c);
    }
    const glm::mat4 rotation =
        light_rotation(frame_.directional[static_cast<std::size_t>(frame_.shadow_light)].direction);
    for (std::uint32_t cascade = 0; cascade < kShadowCascades; ++cascade) {
        // Target first: setting a graphics pipeline re-checks the bound targets.
        const rhi::TextureView dsv = shadow_map_.view({rhi::ViewType::DepthStencil, 0, 1, cascade, 1});
        ctx.set_render_target(rhi::TextureView{}, dsv);
        ctx.clear_depth(dsv, 1.0f);
        ctx.set_pipeline(shadow_pipeline_);
        ctx.update_buffer(shadow_pass_constants_, &frame_.cascades.view_projection[cascade], sizeof(glm::mat4));
        const glm::vec4 sphere = frame_.cascades.spheres[cascade];
        const glm::vec3 centre = glm::vec3(rotation * glm::vec4(glm::vec3(sphere), 1.0f));
        for (const Caster& c : casters) {
            // Light space: outside the cascade's square, or entirely behind its
            // receivers (further from the light), it can't shadow anything there.
            const glm::vec3 p = glm::vec3(rotation * glm::vec4(c.centre, 1.0f));
            if (std::abs(p.x - centre.x) > sphere.w + c.radius || std::abs(p.y - centre.y) > sphere.w + c.radius ||
                p.z + c.radius < centre.z - sphere.w) {
                continue;
            }
            ObjectConstants object;
            object.world = c.world;
            object.normal_matrix = glm::mat4(1.0f);
            ctx.update_buffer(object_constants_, &object, sizeof(object));
            ctx.commit(shadow_bindings_);
            ctx.set_vertex_buffers({c.item.vertices, c.item.uvs});
            ctx.set_index_buffer(*c.item.indices);
            ctx.draw_indexed(c.item.index_count, rhi::IndexType::UInt32);
        }
        draw_terrains(ctx, TerrainPass::Shadow, frame_.cascades.view_projection[cascade]);
    }
}

std::uint32_t RasterEngine::baked_probes() const noexcept {
    return static_cast<std::uint32_t>(
        std::count_if(probes_.begin(), probes_.end(), [](const auto& p) { return p.second.baked; }));
}

std::uint32_t RasterEngine::pending_probes() const noexcept {
    return static_cast<std::uint32_t>(probes_.size()) - baked_probes();
}

void RasterEngine::update_probes(rhi::CommandContext& ctx) {
    if (!world_) {
        probes_.clear();
        return;
    }
    world_->update_transforms();
    for (auto& [key, probe] : probes_) {
        probe.seen = false;
    }
    const auto& registry = world_->registry();
    for (const entt::entity e : world_->depth_first()) {
        const auto* component = registry.try_get<scene::ReflectionProbe>(e);
        if (!component || !scene::is_visible(*world_, e)) {
            continue;
        }
        const glm::vec3 position = glm::vec3(registry.get<scene::Transform>(e).world[3]);
        // Power of two between 32 and 512: every prefiltered mip is at least 1 texel.
        const std::uint32_t resolution = std::bit_ceil(std::clamp(component->resolution, 32u, 512u));
        const auto key = static_cast<std::uint32_t>(entt::to_integral(e));
        auto [it, inserted] = probes_.try_emplace(key);
        Probe& probe = it->second;
        probe.seen = true;
        if (inserted) {
            std::vector<bool> used(kMaxEnvSlots, false);
            for (const auto& [k, p] : probes_) {
                if (k != key && p.slot < kMaxEnvSlots) {
                    used[p.slot] = true;
                }
            }
            probe.slot = 0;
            for (std::uint32_t slot = 1; slot < kMaxEnvSlots; ++slot) {
                if (!used[slot]) {
                    probe.slot = slot;
                    break;
                }
            }
            if (probe.slot == 0) {
                F3D_LOG_WARN("raster: more than {} reflection probes; the rest use the sky", kMaxEnvSlots - 1);
            }
        }
        if (probe.resolution != resolution) {
            probe.capture = sky_.create_source_cube("Probe capture", resolution, true);
            probe.specular = sky_.create_specular_cube("Probe specular", resolution);
            probe.resolution = resolution;
            probe.baked = false;
        }
        if (probe.position != position) {
            probe.position = position;
            probe.baked = false;
        }
        probe.radius = component->radius;
    }
    std::erase_if(probes_, [](const auto& entry) { return !entry.second.seen; });

    // At most one capture per frame.
    for (auto& [key, probe] : probes_) {
        if (probe.slot == 0) {
            continue;
        }
        if (!probe.baked || probe.env_revision != env_revision_ || probe.request != probe_request_) {
            capture_probe(ctx, key);
            break;
        }
    }
}

void RasterEngine::capture_probe(rhi::CommandContext& ctx, std::uint32_t key) {
    F3D_ZONE;
    Probe& probe = probes_.at(key);
    if (!probe.capture || !probe.specular) {
        return;
    }
    rhi::ScopedGpuTimer timer(ctx, "probe_capture");
    const std::uint32_t size = probe.resolution;
    if (!probe_depth_ || probe_depth_.width() != size) {
        probe_depth_ =
            device_.create_texture({"Probe capture depth", size, size, depth_format(), rhi::BindFlags::DepthStencil});
    }
    // D3D cube-face orientation: forward and up of each face (rendered mirrored).
    static const std::array<std::pair<glm::vec3, glm::vec3>, 6> kFaces = {{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    }};
    FrameLighting face_frame = frame_;
    face_frame.shadow_light = -1;          // no cascades for the probe's views
    face_frame.exposure_scale = kEnvScale; // absolute radiance, scaled for fp16
    face_frame.gpu_exposure = false;
    face_frame.view_mode = static_cast<std::uint32_t>(ViewMode::Lit);
    const View main_view = view_;
    const std::array<float, 4> background{0.0f, 0.0f, 0.0f, 1.0f};
    capturing_probe_ = true;
    for (std::uint32_t face = 0; face < 6; ++face) {
        View v;
        v.camera_position = probe.position;
        v.view = glm::lookAtRH(probe.position, probe.position + kFaces[face].first, kFaces[face].second);
        v.near_plane = 0.05f;
        v.far_plane = 2000.0f;
        v.projection = glm::perspectiveRH_ZO(glm::half_pi<float>(), 1.0f, v.near_plane, v.far_plane);
        v.projection[0][0] = -v.projection[0][0]; // cube faces are mirrored relative to a right-handed camera
        v.width = v.height = size;
        view_ = v;
        const ViewConstants constants = make_view_constants(v, grid_, face_frame);
        ctx.update_buffer(view_constants_, &constants, sizeof(constants));
        cluster_lights(ctx);
        const rhi::TextureView rtv = probe.capture.view({rhi::ViewType::RenderTarget, 0, 1, face, 1});
        const rhi::TextureView dsv = probe_depth_.view({rhi::ViewType::DepthStencil});
        ctx.set_render_target(rtv, dsv);
        ctx.clear_color(rtv, background);
        ctx.clear_depth(dsv, 1.0f);
        draw_meshes(ctx);
        if (sky_.physical()) {
            sky_.draw_sky(ctx, view_constants_, exposure_state_);
        }
    }
    capturing_probe_ = false;
    view_ = main_view;
    ctx.generate_mips(probe.capture);
    sky_.prefilter(ctx, probe.capture, probe.specular);
    sky_.project_sh(ctx, probe.capture, probe.slot);
    probe.baked = true;
    probe.env_revision = env_revision_;
    probe.request = probe_request_;
}

// --- Exposure ----------------------------------------------------------------------------------

void RasterEngine::update_exposure(rhi::CommandContext& ctx, float delta) {
    const ExposureSettings& e = settings_.exposure;
    frame_.view_mode = static_cast<std::uint32_t>(settings_.mode);
    const bool lit = settings_.mode == ViewMode::Lit || settings_.mode == ViewMode::Cascades;
    if (sky_constants_.ground_albedo.w < 0.5f) { // set by make_frame_lighting: is there a Sky?
        // No Sky: relative units (P4.2's look), only the compensation applies.
        frame_.gpu_exposure = false;
        frame_.exposure_scale = std::exp2(e.compensation);
        exposure_active_ = false;
        return;
    }
    if (!lit) {
        frame_.gpu_exposure = false;
        frame_.exposure_scale = 1.0f;
        return;
    }
    const float compensation = e.compensation + (cached_sky_valid_ ? cached_sky_.exposure_compensation : 0.0f);
    frame_.gpu_exposure = true;
    frame_.exposure_scale = 1.0f;
    if (exposure_reset_ || !exposure_active_) {
        // Seed with the CPU sky meter; the first metering snaps instead of adapting.
        const float ev =
            e.mode == ExposureMode::Manual ? e.manual_ev100 - compensation : sky_lighting_.ev100 - e.compensation;
        const glm::vec4 seed(exposure_from_ev100(ev), ev, 0.0f, 0.0f);
        ctx.update_buffer(exposure_state_, &seed, sizeof(seed));
        exposure_reset_ = false;
    }
    exposure_active_ = true;
    ExposureConstants c;
    c.params = glm::vec4(kHistogramMinLog2, kHistogramRangeLog2, 1.0f - std::exp(-delta * e.speed_up),
                         1.0f - std::exp(-delta * e.speed_down));
    c.limits = glm::vec4(e.min_ev100, e.max_ev100, e.manual_ev100, e.mode == ExposureMode::Auto ? 1.0f : 0.0f);
    c.extra = glm::vec4(compensation, std::clamp(e.low_percent, 0.0f, 1.0f),
                        std::clamp(e.high_percent, e.low_percent, 1.0f), 0.0f);
    ctx.update_buffer(exposure_constants_, &c, sizeof(c));
}

void RasterEngine::meter_exposure(rhi::CommandContext& ctx, const rhi::Texture& scene_color) {
    F3D_ZONE;
    if (!histogram_pipeline_ || !adapt_pipeline_) {
        return;
    }
    if (!histogram_bindings_ || !(histogram_source_ == scene_color)) {
        histogram_bindings_ = histogram_pipeline_.create_bindings();
        histogram_bindings_.set("ExposureConstants", exposure_constants_);
        histogram_bindings_.set("g_SceneColor", scene_color);
        histogram_bindings_.set("g_Exposure", exposure_state_);
        histogram_bindings_.set("g_Histogram", histogram_);
        histogram_source_ = scene_color;
    }
    ctx.set_pipeline(histogram_pipeline_);
    ctx.commit(histogram_bindings_);
    ctx.dispatch((scene_color.width() + 15) / 16, (scene_color.height() + 15) / 16);
    ctx.set_pipeline(adapt_pipeline_);
    ctx.commit(adapt_bindings_);
    ctx.dispatch(1);
}

glm::vec4 RasterEngine::read_exposure() {
    const auto bytes = device_.context().read_buffer(exposure_state_);
    glm::vec4 out(0.0f);
    if (bytes.size() >= sizeof(out)) {
        std::memcpy(&out, bytes.data(), sizeof(out));
    }
    return out;
}

// --- Frame -------------------------------------------------------------------------------------

void RasterEngine::render(rhi::CommandContext& ctx, const View& output_view, const RenderScene& scene,
                          const RenderTarget& target) {
    F3D_ZONE;
    if (!target.output) {
        return;
    }
    if (shader_generation_ != shaders_.generation()) {
        create_pipelines();
    }
    set_scene(scene);
    if (world_) {
        world::update_landscape(*world_); // roads and riverbeds into the terrain surfaces
    }
    if (world_ && meshes_) {
        modeling::sync_meshes(*world_, *meshes_); // editable meshes -> drawable meshes
    }
    // The scene passes run at the screen percentage of the output size; the resolve scales
    // the result to the output (the projection is the same, only the pixel count differs).
    View view = output_view;
    const glm::uvec2 render_size =
        scaled_render_size(output_view.width, output_view.height, settings_.screen_percentage);
    view.width = render_size.x;
    view.height = render_size.y;
    const bool upscaled = view.width != output_view.width || view.height != output_view.height;
    last_render_size_ = render_size;
    view_ = view;
    const SceneLights lights = world_ ? gather_lights(*world_) : SceneLights{};
    upload_lights(ctx, lights);
    frame_ = make_frame_lighting(view, lights);
    ensure_shadow_map(frame_.shadow_light >= 0);
    const auto now = std::chrono::steady_clock::now();
    float delta = frame_delta_override_;
    if (delta < 0.0f) {
        delta = last_frame_time_ == std::chrono::steady_clock::time_point{}
                    ? 0.0f
                    : std::chrono::duration<float>(now - last_frame_time_).count();
    }
    last_frame_time_ = now;
    update_exposure(ctx, std::clamp(delta, 0.0f, 0.25f));
    prepare_terrains(ctx, view);

    // Sky look-up tables and environment, then reflection probes (they see the sky).
    if (sky_.update(ctx, sky_constants_) || !env_baked_) {
        sky_.bake_environment(ctx);
        env_baked_ = true;
        ++env_revision_;
    }
    update_probes(ctx);

    const ViewConstants constants = make_view_constants(view, grid_, frame_);
    ctx.update_buffer(view_constants_, &constants, sizeof(constants));

    const bool lit = settings_.mode == ViewMode::Lit || settings_.mode == ViewMode::Cascades;
    const bool fog = lit && fog_.prepare(ctx, view, world_, frame_.directional, settings_.fog_quality, sky_.physical());
    last_fog_grid_ = fog && fog_.volumetric() ? fog_.grid_size() : glm::uvec3(0u);
    const std::optional<scene::PostProcess> post =
        lit && settings_.post_process && world_ ? gather_post_process(*world_) : std::nullopt;

    auto graph = std::make_unique<graph::RenderGraph>();
    graph::RenderGraph& g = *graph;
    const graph::TextureHandle output = g.import_texture("Output", target.output, rhi::ResourceState::ShaderResource);
    graph::TextureHandle color = g.create_texture("Scene colour", {view.width, view.height, color_format()});
    graph::TextureHandle depth = g.create_texture("Depth", {view.width, view.height, depth_format()});

    // Light lists per cluster. Buffers aren't graph resources: the pass is a side
    // effect and runs before the mesh pass that reads them (declaration order).
    g.add_pass(
        "light_cluster", [](graph::PassBuilder& b) { b.side_effect(); },
        [this](graph::PassContext& pc) { cluster_lights(pc.cmd); });

    // Shadow cascades of the shadowing directional light.
    graph::TextureHandle shadows{};
    const bool has_shadows = frame_.shadow_light >= 0 && shadow_map_;
    if (has_shadows) {
        const graph::TextureHandle imported =
            g.import_texture("Shadow map", shadow_map_, rhi::ResourceState::ShaderResource);
        struct Shadows {
            graph::TextureHandle map;
        };
        shadows = g.add_pass<Shadows>(
                       "shadows",
                       [&](graph::PassBuilder& b, Shadows& d) { d.map = b.write(imported, graph::Access::DepthWrite); },
                       [this](const Shadows&, graph::PassContext& pc) { draw_shadows(pc.cmd); })
                      .map;
    }

    // Opaque geometry first: the sky, grid and lines depth-test against it.
    struct Opaque {
        graph::TextureHandle color, depth;
    };
    const Opaque& opaque = g.add_pass<Opaque>(
        "mesh",
        [&](graph::PassBuilder& b, Opaque& d) {
            if (has_shadows) {
                b.read(shadows);
            }
            d.color = b.write(color);
            d.depth = b.write(depth, graph::Access::DepthWrite);
        },
        [this](const Opaque& d, graph::PassContext& pc) {
            pc.cmd.set_render_target(pc.texture(d.color), pc.texture(d.depth));
            // Alpha 0 marks the background: the resolve fills it with the clear colour.
            pc.cmd.clear_color(pc.texture(d.color), {0.0f, 0.0f, 0.0f, 0.0f});
            pc.cmd.clear_depth(pc.texture(d.depth), 1.0f);
            draw_meshes(pc.cmd);
            draw_terrains(pc.cmd, settings_.mode == ViewMode::Wireframe ? TerrainPass::Wireframe : TerrainPass::Lit,
                          view_.view_projection());
        });
    color = opaque.color;
    depth = opaque.depth;

    if (sky_.physical() && lit) {
        struct Sky {
            graph::TextureHandle color, depth;
        };
        color = g.add_pass<Sky>(
                     "sky",
                     [&](graph::PassBuilder& b, Sky& d) {
                         d.color = b.write(color);
                         d.depth = b.read(depth, graph::Access::DepthTest);
                     },
                     [this](const Sky& d, graph::PassContext& pc) {
                         pc.cmd.set_render_target(pc.texture(d.color), pc.texture(d.depth));
                         sky_.draw_sky(pc.cmd, view_constants_, exposure_state_);
                     })
                    .color;
    }

    // Water (P7.3): over a copy of the scene colour and depth (refraction, absorption by
    // depth, screen-space reflections), before the fog so it is fogged like the rest.
    if (lit && prepare_water(std::clamp(delta, 0.0f, 0.25f))) {
        struct WaterCopy {
            graph::TextureHandle color, depth, color_copy, depth_copy;
        };
        const graph::TextureHandle color_copy =
            g.create_texture("Scene colour copy", {view.width, view.height, color_format()});
        const graph::TextureHandle depth_copy =
            g.create_texture("Depth copy", {view.width, view.height, rhi::Format::R32Float});
        const WaterCopy& copies = g.add_pass<WaterCopy>(
            "water_copy",
            [&](graph::PassBuilder& b, WaterCopy& d) {
                d.color = b.read(color);
                d.depth = b.read(depth);
                d.color_copy = b.write(color_copy);
                d.depth_copy = b.write(depth_copy);
            },
            [this](const WaterCopy& d, graph::PassContext& pc) {
                copy_for_water(pc.cmd, pc.texture(d.color), pc.texture(d.depth), pc.texture(d.color_copy),
                               pc.texture(d.depth_copy));
            });
        struct Water {
            graph::TextureHandle color, depth, color_copy, depth_copy;
        };
        const Water& water = g.add_pass<Water>(
            "water",
            [&](graph::PassBuilder& b, Water& d) {
                if (has_shadows) {
                    b.read(shadows);
                }
                d.color_copy = b.read(copies.color_copy);
                d.depth_copy = b.read(copies.depth_copy);
                d.color = b.write(copies.color);
                d.depth = b.write(copies.depth, graph::Access::DepthWrite);
            },
            [this](const Water& d, graph::PassContext& pc) {
                pc.cmd.set_render_target(pc.texture(d.color), pc.texture(d.depth));
                draw_water(pc.cmd, pc.texture(d.color_copy), pc.texture(d.depth_copy));
            });
        color = water.color;
        depth = water.depth;
    } else {
        water_draws_.clear();
    }

    // Fog: the froxel volume (lit with the shadow map and the light clusters), then the
    // composite over the scene, sky included.
    if (fog) {
        const FogRenderer::Inputs inputs{&view_constants_,   &lights_,          &cluster_counts_, &cluster_indices_,
                                         &sky_.sh_buffer(), &shadow_map_, &exposure_state_};
        if (fog_.volumetric()) {
            g.add_pass(
                "fog_volume",
                [&](graph::PassBuilder& b) {
                    if (has_shadows) {
                        b.read(shadows);
                    }
                    b.side_effect();
                },
                [this, inputs](graph::PassContext& pc) { fog_.compute(pc.cmd, inputs); });
        }
        struct Fog {
            graph::TextureHandle color, depth;
        };
        color = g.add_pass<Fog>(
                     "fog",
                     [&](graph::PassBuilder& b, Fog& d) {
                         d.depth = b.read(depth);
                         d.color = b.write(color);
                     },
                     [this, inputs](const Fog& d, graph::PassContext& pc) {
                         pc.cmd.set_render_target(pc.texture(d.color));
                         fog_.apply(pc.cmd, inputs, pc.texture(d.depth));
                     })
                    .color;
    }

    // Automatic exposure: meter this frame's scene colour for the next one.
    if (frame_.gpu_exposure) {
        g.add_pass(
            "exposure",
            [&](graph::PassBuilder& b) {
                b.read(color);
                b.side_effect();
            },
            [this, color](graph::PassContext& pc) { meter_exposure(pc.cmd, pc.texture(color)); });
    }

    // Bloom: blurred levels outside the graph, read by the resolve.
    bool bloom = post && post->bloom_intensity > 0.0f;
    if (bloom) {
        g.add_pass(
            "bloom",
            [&](graph::PassBuilder& b) {
                b.read(color);
                b.side_effect();
            },
            [this, color, settings = *post](graph::PassContext& pc) {
                bloom_.render(pc.cmd, pc.texture(color), settings);
            });
    }

    ResolveConstants resolve_constants;
    resolve_constants.mode = glm::uvec4(static_cast<std::uint32_t>(lit ? settings_.tonemapper : Tonemapper::None),
                                        settings_.mode == ViewMode::Normals ? 0u : 1u, 0u, ++resolve_frame_);
    resolve_constants.clear = glm::vec4(target.clear_color[0], target.clear_color[1], target.clear_color[2], 1.0f);
    resolve_constants.lens = glm::vec4(view.orthographic ? 0.0f : 1.0f / view.projection[0][0],
                                       view.orthographic ? 0.0f : 1.0f / view.projection[1][1],
                                       1.0f / static_cast<float>(view.width), 1.0f / static_cast<float>(view.height));
    if (upscaled) {
        resolve_constants.mode.z |= kResolveUpscale;
    }
    if (bloom) {
        resolve_constants.mode.z |= kResolveBloom;
        resolve_constants.bloom = glm::vec4(std::clamp(post->bloom_intensity, 0.0f, 1.0f),
                                            1.0f / static_cast<float>(bloom_levels(post->bloom_radius, view.width,
                                                                                   view.height)),
                                            0.0f, 0.0f);
    }
    if (post) {
        resolve_constants.mode.z |= kResolveGrade;
        resolve_constants.grade = glm::vec4(std::clamp(post->vignette, 0.0f, 1.0f), std::max(post->saturation, 0.0f),
                                            std::max(post->contrast, 0.01f), std::clamp(post->film_grain, 0.0f, 1.0f));
        const glm::mat3 balance = white_balance(post->temperature, post->tint);
        for (glm::length_t row = 0; row < 3; ++row) {
            resolve_constants.white_balance[row] = glm::vec4(balance[0][row], balance[1][row], balance[2][row], 0.0f);
        }
    }

    struct Resolve {
        graph::TextureHandle color, output;
    };
    graph::TextureHandle display =
        g.add_pass<Resolve>(
             "resolve",
             [&](graph::PassBuilder& b, Resolve& d) {
                 d.color = b.read(color);
                 d.output = b.write(output);
             },
             [this, resolve_constants, bloom](const Resolve& d, graph::PassContext& pc) {
                 if (!resolve_pipeline_) {
                     return;
                 }
                 const rhi::Texture& source = pc.texture(d.color);
                 const rhi::Texture& glow = bloom ? bloom_.result() : white_;
                 pc.cmd.set_render_target(pc.texture(d.output));
                 // Another transient texture (resize), or bloom turned on or off.
                 if (!(resolve_source_ == source) || !(resolve_bloom_ == glow)) {
                     resolve_bindings_ = resolve_pipeline_.create_bindings();
                     resolve_bindings_.set("g_SceneColor", source);
                     resolve_bindings_.set("g_Bloom", glow);
                     resolve_bindings_.set("ResolveConstants", resolve_constants_);
                     resolve_source_ = source;
                     resolve_bloom_ = glow;
                 }
                 pc.cmd.update_buffer(resolve_constants_, &resolve_constants, sizeof(resolve_constants));
                 pc.cmd.set_pipeline(resolve_pipeline_);
                 pc.cmd.commit(resolve_bindings_);
                 pc.cmd.draw(3);
             })
            .output;

    // Overlays on the tonemapped output, tested against (not writing) the scene depth,
    // so their colours are exact whatever the exposure. Below 100 % the scene depth is
    // first copied to an output-sized target, and the overlays get output-sized view constants.
    const bool overlays = (grid_.visible && grid_pipeline_) || debug_lines_;
    graph::TextureHandle overlay_depth = depth;
    if (upscaled && overlays && depth_upscale_pipeline_) {
        struct DepthUpscale {
            graph::TextureHandle source, target;
        };
        const graph::TextureHandle full =
            g.create_texture("Output depth", {output_view.width, output_view.height, depth_format()});
        const ViewConstants output_constants = make_view_constants(output_view, grid_, frame_);
        overlay_depth = g.add_pass<DepthUpscale>(
                             "depth_upscale",
                             [&](graph::PassBuilder& b, DepthUpscale& d) {
                                 d.source = b.read(depth);
                                 d.target = b.write(full, graph::Access::DepthWrite);
                             },
                             [this, output_constants](const DepthUpscale& d, graph::PassContext& pc) {
                                 const rhi::Texture& source = pc.texture(d.source);
                                 if (!(depth_upscale_source_ == source)) {
                                     depth_upscale_bindings_ = depth_upscale_pipeline_.create_bindings();
                                     depth_upscale_bindings_.set("g_SceneDepth", source);
                                     depth_upscale_source_ = source;
                                 }
                                 pc.cmd.update_buffer(view_constants_, &output_constants, sizeof(output_constants));
                                 pc.cmd.set_render_target(rhi::Texture{}, pc.texture(d.target));
                                 pc.cmd.set_pipeline(depth_upscale_pipeline_);
                                 pc.cmd.commit(depth_upscale_bindings_);
                                 pc.cmd.draw(3);
                             })
                            .target;
    }
    struct Overlay {
        graph::TextureHandle output, depth;
    };
    const auto overlay = [&](const char* name, auto draw) {
        display = g.add_pass<Overlay>(
                       name,
                       [&](graph::PassBuilder& b, Overlay& d) {
                           d.output = b.write(display);
                           d.depth = b.read(overlay_depth, graph::Access::DepthTest);
                       },
                       [draw](const Overlay& d, graph::PassContext& pc) {
                           pc.cmd.set_render_target(pc.texture(d.output), pc.texture(d.depth));
                           draw(pc.cmd);
                       })
                      .output;
    };
    if (grid_.visible && grid_pipeline_) {
        overlay("grid", [this](rhi::CommandContext& cmd) {
            cmd.set_pipeline(grid_pipeline_);
            cmd.commit(grid_bindings_);
            cmd.draw(3);
        });
    }
    if (debug_lines_) {
        overlay("debug_lines", [this](rhi::CommandContext& cmd) { draw_lines(cmd); });
    }

    g.compile();
    g.execute(ctx, transients_);
    transients_.end_frame();
    last_graph_ = std::move(graph);
}

// --- Picking -----------------------------------------------------------------------------------

entt::entity RasterEngine::entity_from_id(std::uint32_t id) noexcept {
    if (id == 0) {
        return entt::null;
    }
    return entt::entity{static_cast<std::uint32_t>(id - 1u)};
}

void RasterEngine::render_pick(const View& view, const RenderScene& scene) {
    F3D_ZONE;
    if (shader_generation_ != shaders_.generation()) {
        create_pipelines();
    }
    set_scene(scene);
    if (world_) {
        world::update_landscape(*world_); // roads and riverbeds into the terrain surfaces
    }
    if (!pick_pipeline_ || !world_ || !meshes_) {
        return;
    }
    rhi::CommandContext& ctx = device_.context();
    if (!pick_target_ || pick_target_.width() != view.width || pick_target_.height() != view.height) {
        pick_target_ = device_.create_texture({"Pick ID", view.width, view.height, rhi::Format::R32Uint,
                                               rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
        if (!pick_target_) {
            return;
        }
    }
    view_ = view;
    prepare_terrains(ctx, view);
    const ViewConstants constants = make_view_constants(view, grid_);
    ctx.update_buffer(view_constants_, &constants, sizeof(constants));

    // The ID target is read back after the graph ran, so it is imported (an output).
    graph::RenderGraph g;
    const graph::TextureHandle ids = g.import_texture("Pick ID", pick_target_);
    const graph::TextureHandle depth = g.create_texture("Pick depth", {view.width, view.height, depth_format()});
    struct Pick {
        graph::TextureHandle ids, depth;
    };
    g.add_pass<Pick>(
        "pick",
        [&](graph::PassBuilder& b, Pick& d) {
            d.ids = b.write(ids);
            d.depth = b.write(depth, graph::Access::DepthWrite);
        },
        [this](const Pick& d, graph::PassContext& pc) { draw_pick(pc.cmd, pc.texture(d.ids), pc.texture(d.depth)); });
    g.execute(ctx, transients_, false);
    transients_.end_frame();
}

void RasterEngine::draw_pick(rhi::CommandContext& ctx, const rhi::Texture& ids, const rhi::Texture& depth) {
    ctx.set_render_target(ids, depth);
    ctx.clear_color_uint(ids, 0u);
    ctx.clear_depth(depth, 1.0f);

    // Hidden and locked layers stay out of the ID buffer, so what is behind them can be picked.
    collect_draws(DrawPass::Pick, draws_);
    for (const MeshDraw& d : draws_) {
        const entt::entity e = d.entity;
        const glm::mat4& world = d.world;
        DrawItem item;
        if (!resolve(*d.renderer, world, item)) {
            continue;
        }
        ObjectConstants object;
        object.world = world;
        object.normal_matrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world))));
        object.object_id = glm::uvec4(static_cast<std::uint32_t>(entt::to_integral(e)) + 1u, 0u, 0u, 0u);
        ctx.update_buffer(object_constants_, &object, sizeof(object));
        ctx.set_pipeline(pick_pipeline_);
        ctx.commit(pick_bindings_);
        ctx.set_vertex_buffers({item.vertices, item.uvs});
        ctx.set_index_buffer(*item.indices);
        ctx.draw_indexed(item.index_count, rhi::IndexType::UInt32);
    }
    draw_terrains(ctx, TerrainPass::Pick, view_.view_projection());
}

entt::entity RasterEngine::pick(const View& view, const RenderScene& scene, std::uint32_t x, std::uint32_t y) {
    render_pick(view, scene);
    if (!pick_target_) {
        return entt::null;
    }
    const std::vector<std::uint32_t> data = device_.context().read_texture_uint32(pick_target_);
    const std::size_t index = static_cast<std::size_t>(y) * view.width + x;
    if (index >= data.size()) {
        return entt::null;
    }
    return entity_from_id(data[index]);
}

std::vector<entt::entity> RasterEngine::pick_region(const View& view, const RenderScene& scene, std::uint32_t x0,
                                                    std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    render_pick(view, scene);
    std::vector<entt::entity> out;
    if (!pick_target_) {
        return out;
    }
    const std::vector<std::uint32_t> data = device_.context().read_texture_uint32(pick_target_);
    x1 = std::min(x1, view.width);
    y1 = std::min(y1, view.height);
    std::unordered_set<std::uint32_t> seen;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * view.width + x;
            if (index >= data.size()) {
                continue;
            }
            const std::uint32_t id = data[index];
            if (id != 0u && seen.insert(id).second) {
                if (const entt::entity e = entity_from_id(id); e != entt::null) {
                    out.push_back(e);
                }
            }
        }
    }
    return out;
}

} // namespace f3d::render::raster
