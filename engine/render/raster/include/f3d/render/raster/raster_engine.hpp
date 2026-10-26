// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/assets/mesh_library.hpp"
#include "f3d/render/graph/render_graph.hpp"
#include "f3d/render/raster/atmosphere.hpp"
#include "f3d/render/raster/fog.hpp"
#include "f3d/render/raster/lights.hpp"
#include "f3d/render/raster/post_process.hpp"
#include "f3d/render/raster/render_settings.hpp"
#include "f3d/render/raster/residency.hpp"
#include "f3d/render/raster/shadows.hpp"
#include "f3d/render/raster/sky.hpp"
#include "f3d/render/raster/view.hpp"
#include "f3d/render/render_engine.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/components.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3d::terrain {
struct Terrain;
}

namespace f3d::render::raster {

/// GPU layout of shaders/object.hlsli `ObjectConstants`.
struct ObjectConstants {
    glm::mat4 world;
    glm::mat4 normal_matrix;
    glm::uvec4 object_id; ///< x: entity index + 1, y: environment slot (0 sky, then probes)
};
static_assert(sizeof(ObjectConstants) == 2 * 64 + 16);

/// Flags of shaders/material.hlsli.
enum MaterialFlags : std::uint32_t {
    kMaterialNormalMap = 1u,
    kMaterialOrmOcclusion = 2u,
    kMaterialAlphaMask = 4u,
    kMaterialOcclusionMap = 8u,
};

/// GPU layout of shaders/exposure.hlsli `ExposureConstants`.
struct ExposureConstants {
    glm::vec4 params; ///< log2 luminance at bin 1, log2 range, adaptation rising, adaptation falling
    glm::vec4 limits; ///< min EV100, max EV100, manual EV100, 1 = automatic
    glm::vec4 extra;  ///< compensation, low percentile, high percentile, 1 = snap
};

/// GPU layout of shaders/resolve.ps.hlsl `ResolveConstants`.
struct ResolveConstants {
    glm::uvec4 mode{0u}; ///< x: tonemapper, y: 1 = sRGB encode, z: kResolve* flags, w: frame index
    glm::vec4 clear{};   ///< linear background colour (not tonemapped)
    glm::vec4 bloom{0.0f};                   ///< x: intensity, y: 1 / levels
    glm::vec4 grade{0.0f, 1.0f, 1.0f, 0.0f}; ///< vignette, saturation, contrast, film grain
    glm::vec4 white_balance[3] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
    glm::vec4 lens{0.0f}; ///< xy: tangent of half the field of view, zw: scene texel size
};

/// ResolveConstants::mode.z flags.
enum ResolveFlags : std::uint32_t {
    kResolveUpscale = 1u, ///< the scene is smaller than the output (screen percentage)
    kResolveBloom = 2u,
    kResolveGrade = 4u,
};

/// GPU layout of shaders/terrain.vs.hlsl `TerrainConstants`.
struct TerrainConstants {
    glm::mat4 view_projection;
    glm::vec4 origin;    ///< xyz: world origin of terrain-local space, w: sample spacing
    glm::vec4 params;    ///< samples, size, height offset, height scale (R16) or 0 (R32F)
    glm::vec4 camera;    ///< xyz: terrain-local camera
    glm::vec4 draw;      ///< x: first node of this draw
    glm::vec4 morph[12]; ///< per LOD: morph start, 1 / (end - start)
};

/// GPU layout of shaders/terrain.hlsli `TerrainLayerConstants` (P6.4).
struct TerrainLayerConstants {
    glm::vec4 color[16];   ///< rgb: linear albedo (or the albedo texture's tint), a: roughness
    glm::vec4 params[16];  ///< x: 1 / tiling, y: triplanar, z: metallic, w: normal strength
    glm::vec4 params2[16]; ///< x: height contrast, y: anti-tiling
    glm::uvec4 flags[16];  ///< x: kLayer* flags (the textures the layer's slice holds)
    glm::uvec4 info;       ///< x: layer count, y: splat maps
};

/// TerrainLayerConstants::flags bits (shaders/terrain.hlsli).
enum TerrainLayerFlags : std::uint32_t {
    kLayerAlbedo = 1u,
    kLayerNormal = 2u,
    kLayerRoughness = 4u,
    kLayerAo = 8u,
    kLayerHeight = 16u,
};

/// GPU layout of shaders/water.hlsli `WaterConstants` (P7.3).
struct WaterConstants {
    glm::vec4 shallow{0.0f}; ///< rgb: linear shallow colour, a: clarity (metres)
    glm::vec4 deep{0.0f};    ///< rgb: linear deep colour, a: foam
    glm::vec4 waves{0.0f};   ///< height, length, speed, wind direction (radians)
    glm::vec4 params{0.0f};  ///< refraction, roughness, time (seconds), kind (0 lake, 1 river, 2 ocean)
    glm::vec4 ocean{0.0f};   ///< xy: grid centre (world x, z), z: water level
};

/// Luminance range of the exposure histogram (log2 cd/m2): starlight to the sun's glare.
inline constexpr float kHistogramMinLog2 = -10.0f;
inline constexpr float kHistogramRangeLog2 = 30.0f;

/// GPU layout of shaders/material.hlsli `MaterialConstants`.
struct MaterialConstants {
    glm::vec4 base_color{1.0f};
    glm::vec4 emissive{0.0f}; ///< rgb: emissive x strength
    float metallic = 0.0f;
    float roughness = 0.5f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float alpha_cutoff = 0.5f;
    std::uint32_t flags = 0;
    float pad[2] = {0.0f, 0.0f};
};
static_assert(sizeof(MaterialConstants) == 64);

/// The raster IRenderEngine (design §3.G): clustered forward+ with metal/rough
/// PBR materials (phases.md P4.2), a physical sky, image-based lighting and
/// cascaded shadow maps (P4.3). Before the graph: the sky's LUTs, its environment
/// when it changed, and at most one reflection-probe capture. Each frame is a
/// render graph:
///   light_cluster (compute: local lights -> froxel light lists)
///   shadows       (four cascades of the shadowing directional light)
///   mesh          (MeshRenderers, one draw per submesh/material, into pre-exposed HDR colour + depth)
///   sky           (with a Sky entity: the background behind the geometry)
///   fog_volume    (volumetric fog: light the froxels, integrate them; fog.hpp)
///   fog           (height fog and the volumetric fog over the scene colour)
///   exposure      (with a Sky: luminance histogram -> adapted exposure for the next frame)
///   bloom         (with a PostProcess: the blurred glow, post_process.hpp)
///   resolve       (upscaling, bloom, grading, tonemapping: HDR -> the display output)
///   depth_upscale (below 100 % screen percentage: an output-sized depth for the overlays)
///   grid, debug_lines (overlays on the display output, depth-tested)
///
/// The scene passes run at RenderSettings::screen_percentage of the output size.
///
/// The output texture is UNORM (not an sRGB format) on purpose: the UI samples it
/// as already-encoded colour, exactly like its other sRGB-authored colours.
class RasterEngine final : public IRenderEngine {
public:
    RasterEngine(rhi::Device& device, rhi::ShaderLibrary& shaders);

    [[nodiscard]] std::string_view name() const noexcept override { return "raster"; }
    /// Draws every entity of `scene` with MeshRenderer + Transform, lit by its Lights.
    void render(rhi::CommandContext& ctx, const View& view, const RenderScene& scene,
                const RenderTarget& target) override;
    /// Streams project assets (MeshRenderers with a GUID mesh, GUID textures)
    /// through `residency` (may be null: GUID meshes come from the MeshLibrary then).
    void set_residency(ResidencyManager* residency) noexcept { residency_ = residency; }

    /// Renders the ID buffer and returns the entity under pixel (x, y), or null.
    /// Entities on hidden or locked layers are not in the ID buffer. Synchronous
    /// (waits for the GPU); meant for mouse clicks.
    entt::entity pick(const View& view, const RenderScene& scene, std::uint32_t x, std::uint32_t y);

    /// The distinct entities inside the rectangle [x0, x1) x [y0, y1). Synchronous.
    std::vector<entt::entity> pick_region(const View& view, const RenderScene& scene, std::uint32_t x0,
                                          std::uint32_t y0, std::uint32_t x1, std::uint32_t y1);
    /// Reconstructs the entity whose integral id was written to the ID buffer (0 = none).
    static entt::entity entity_from_id(std::uint32_t id) noexcept;

    [[nodiscard]] GridSettings& grid() noexcept { return grid_; }
    /// Whether render() draws (and drains) the global debug lines. Off for
    /// secondary renderers (thumbnails) so they don't take the editor's lines.
    void set_debug_lines(bool enabled) noexcept { debug_lines_ = enabled; }
    [[nodiscard]] SimpleLighting& lighting() noexcept { return lighting_; }
    [[nodiscard]] ShadowSettings& shadow_settings() noexcept { return shadow_settings_; }
    /// View mode, tonemapper and exposure (phases.md P4.4).
    [[nodiscard]] RenderSettings& settings() noexcept { return settings_; }
    /// Snaps the automatic exposure to the next frame's meter reading (no adaptation):
    /// after loading a scene, or for captures.
    void reset_exposure() noexcept { exposure_reset_ = true; }
    /// Seconds per frame used for exposure adaptation; negative (the default): measured.
    void set_frame_delta(float seconds) noexcept { frame_delta_override_ = seconds; }
    /// The automatic exposure state: x exposure, y EV100, z 1 if metered. Waits for the GPU.
    [[nodiscard]] glm::vec4 read_exposure();
    [[nodiscard]] SkyRenderer& sky() noexcept { return sky_; }
    /// The last frame's lights, shadow cascades and exposure.
    [[nodiscard]] const FrameLighting& frame_lighting() const noexcept { return frame_; }
    /// The shadow map (a 2D array, one slice per cascade); empty before shadows were drawn.
    [[nodiscard]] const rhi::Texture& shadow_map() const noexcept { return shadow_map_; }
    /// Captures every reflection probe again over the next frames (one per frame).
    void rebake_probes() noexcept { ++probe_request_; }
    /// Reflection probes captured and usable, and probes still waiting for a capture.
    [[nodiscard]] std::uint32_t baked_probes() const noexcept;
    [[nodiscard]] std::uint32_t pending_probes() const noexcept;
    [[nodiscard]] static constexpr rhi::Format color_format() noexcept { return rhi::Format::RGBA16Float; }
    [[nodiscard]] static constexpr rhi::Format depth_format() noexcept { return rhi::Format::D32Float; }
    [[nodiscard]] static constexpr rhi::Format display_format() noexcept { return rhi::Format::RGBA8Unorm; }
    /// Number of debug-line vertices drawn last frame.
    [[nodiscard]] std::uint32_t last_line_vertices() const noexcept { return last_line_vertices_; }
    /// Number of mesh draw calls last frame (one per submesh).
    [[nodiscard]] std::uint32_t last_mesh_draws() const noexcept { return last_mesh_draws_; }
    /// Terrain nodes drawn last frame (camera selection).
    [[nodiscard]] std::uint32_t last_terrain_nodes() const noexcept { return last_terrain_nodes_; }
    /// Local lights clustered last frame.
    [[nodiscard]] std::uint32_t last_local_lights() const noexcept { return last_local_lights_; }
    /// The size the scene was rendered at last frame (the output size times the screen percentage).
    [[nodiscard]] glm::uvec2 last_render_size() const noexcept { return last_render_size_; }
    /// The volumetric fog's froxel grid last frame ({0, 0, 0}: none).
    [[nodiscard]] glm::uvec3 last_fog_grid() const noexcept { return last_fog_grid_; }
    /// The last frame's render graph as Graphviz (dot) text; empty before the first frame.
    [[nodiscard]] std::string graphviz() const { return last_graph_ ? last_graph_->to_graphviz() : std::string{}; }
    /// GPU textures currently held for transient graph resources.
    [[nodiscard]] std::size_t transient_textures() const noexcept { return transients_.size(); }
    /// The light-cluster buffers of the last frame (tests compare them with assign_lights()).
    [[nodiscard]] const rhi::Buffer& cluster_counts() const noexcept { return cluster_counts_; }
    [[nodiscard]] const rhi::Buffer& cluster_indices() const noexcept { return cluster_indices_; }
    /// Drops GPU copies of images loaded by path (a model file changed on disk).
    void clear_texture_cache();

private:
    void create_pipelines();
    void draw_lines(rhi::CommandContext& ctx);
    void draw_meshes(rhi::CommandContext& ctx);
    void cluster_lights(rhi::CommandContext& ctx);
    void upload_lights(rhi::CommandContext& ctx, const SceneLights& lights);
    void render_pick(const View& view, const RenderScene& scene);
    [[nodiscard]] FrameLighting make_frame_lighting(const View& view, const SceneLights& lights);
    void draw_shadows(rhi::CommandContext& ctx);
    void update_probes(rhi::CommandContext& ctx);
    void capture_probe(rhi::CommandContext& ctx, std::uint32_t key);
    void ensure_shadow_map(bool needed);

    // Water (P7.3, raster_water.cpp).
    void create_water_pipelines();
    /// Gathers this frame's water surfaces (lakes, oceans, rivers) and uploads their
    /// meshes; false if there are none.
    bool prepare_water(float delta);
    /// Copies the scene colour and depth into `color_copy` and `depth_copy` (R32F) for the water pass.
    void copy_for_water(rhi::CommandContext& ctx, const rhi::Texture& color, const rhi::Texture& depth,
                        const rhi::Texture& color_copy, const rhi::Texture& depth_copy);
    /// Draws the water over the scene (targets set), refracting the copies.
    void draw_water(rhi::CommandContext& ctx, const rhi::Texture& color_copy, const rhi::Texture& depth_copy);
    /// Last frame's water draws (tests, stats).
    [[nodiscard]] std::size_t last_water_draws() const noexcept { return water_draws_.size(); }

    // Terrain (P6.1, raster_terrain.cpp).
    enum class TerrainPass : std::uint8_t { Lit, Wireframe, Shadow, Pick };
    void create_terrain_pipelines(const std::vector<rhi::VertexAttribute>& unused);
    /// Uploads changed heights and selects every terrain's LODs for `view`.
    void prepare_terrains(rhi::CommandContext& ctx, const View& view);
    /// Draws the prepared terrains (render targets and, for the lit passes, the frame's
    /// bindings already set up). `view_projection`: the pass's (camera or cascade).
    void draw_terrains(rhi::CommandContext& ctx, TerrainPass pass, const glm::mat4& view_projection);
    struct GpuTerrain;
    /// (Re)builds a terrain's layer texture arrays when its layers' textures changed.
    void update_layer_textures(rhi::CommandContext& ctx, GpuTerrain& gpu, const terrain::Terrain& terrain);
    /// One channel set of a layer image resampled to `size` (cached by reference and size).
    const std::vector<std::uint8_t>* layer_image(const core::AssetRef& ref, std::uint32_t size);
    void update_exposure(rhi::CommandContext& ctx, float delta);
    void meter_exposure(rhi::CommandContext& ctx, const rhi::Texture& scene_color);
    void draw_pick(rhi::CommandContext& ctx, const rhi::Texture& ids, const rhi::Texture& depth);
    void set_scene(const RenderScene& scene) noexcept;

    struct GpuMesh {
        std::weak_ptr<const assets::MeshData> source; ///< re-uploaded if the CPU mesh changes
        rhi::Buffer vertices;
        rhi::Buffer uvs;
        rhi::Buffer indices;
        std::uint32_t index_count = 0;
        std::vector<assets::SubMesh> submeshes;
    };
    const GpuMesh* gpu_mesh(const core::AssetRef& ref);
    /// What one MeshRenderer draws this frame.
    struct DrawItem {
        const rhi::Buffer* vertices = nullptr;
        const rhi::Buffer* uvs = nullptr;
        const rhi::Buffer* indices = nullptr;
        std::uint32_t index_count = 0;
        const std::vector<assets::SubMesh>* submeshes = nullptr;
        float screen_px = 0.0f;
        core::Bounds bounds; ///< local
    };
    bool resolve(const scene::MeshRenderer& renderer, const glm::mat4& world, DrawItem& item);
    [[nodiscard]] float screen_radius(const core::Bounds& local_bounds, const glm::mat4& world) const noexcept;

    /// One mesh to draw: a MeshRenderer entity, or one instance of a Scatter entity
    /// (`entity` is the scatter, so picking an instance selects it).
    struct MeshDraw {
        entt::entity entity = entt::null;
        const scene::MeshRenderer* renderer = nullptr;
        glm::mat4 world{1.0f};
    };
    enum class DrawPass : std::uint8_t { Lit, Shadow, Pick };
    /// The meshes of a pass in scene order: visible ones (pickable for Pick), scatter
    /// instances culled by their draw distance (and, except for shadows, the view frustum).
    void collect_draws(DrawPass pass, std::vector<MeshDraw>& out);
    /// Spline surfaces (ribbons) and the meshes along splines (part of collect_draws).
    void collect_spline_draws(DrawPass pass, std::vector<MeshDraw>& out);
    /// Local bounds of a mesh, or empty if unknown (not loaded yet).
    [[nodiscard]] core::Bounds mesh_bounds(const core::AssetRef& ref);

    /// The textures of one draw: base colour, normal, ORM, occlusion, emissive.
    using TextureSet = std::array<rhi::Texture, 5>;
    enum class Variant : std::uint8_t { Normal, Mirrored, DoubleSided, Wireframe };
    /// A material resolved for drawing: constants and textures (defaults where missing).
    struct DrawMaterial {
        MaterialConstants constants;
        TextureSet textures;
        bool double_sided = false;
    };
    [[nodiscard]] DrawMaterial draw_material(const assets::Material* material, const core::AssetRef& texture_override,
                                             float screen_px);
    /// A texture slot: GUID references stream through residency, path references are
    /// model images decoded and uploaded once. Empty texture if unavailable (yet).
    [[nodiscard]] rhi::Texture slot_texture(const core::AssetRef& ref, float screen_px);
    const rhi::Bindings& bindings_for(const TextureSet& textures, const rhi::Texture& environment, Variant variant);
    /// The environment (SH slot, specular cube) for an object centred at `centre`.
    struct Environment {
        std::uint32_t slot = 0;
        const rhi::Texture* specular = nullptr;
    };
    [[nodiscard]] Environment environment_at(const glm::vec3& centre) const;
    struct CachedBindings {
        rhi::Bindings bindings;
        std::uint64_t last_frame = 0;
    };
    void create_default_textures();

    rhi::Device& device_;
    rhi::ShaderLibrary& shaders_;
    std::uint64_t shader_generation_ = ~0ull;
    GridSettings grid_;
    SimpleLighting lighting_;

    rhi::Buffer view_constants_;
    rhi::Pipeline grid_pipeline_;
    rhi::Bindings grid_bindings_;
    rhi::Pipeline line_pipeline_;
    rhi::Bindings line_bindings_;
    rhi::Pipeline resolve_pipeline_;
    rhi::Bindings resolve_bindings_;
    rhi::Texture resolve_source_; ///< scene colour the resolve bindings point at (transient, usually reused)
    rhi::Texture resolve_bloom_;  ///< bloom texture they point at
    rhi::Buffer resolve_constants_;
    rhi::Pipeline depth_upscale_pipeline_;
    rhi::Bindings depth_upscale_bindings_;
    rhi::Texture depth_upscale_source_;
    FogRenderer fog_;
    BloomRenderer bloom_;
    glm::uvec2 last_render_size_{0u};
    glm::uvec3 last_fog_grid_{0u};
    std::uint32_t resolve_frame_ = 0;

    // Exposure and view mode.
    RenderSettings settings_;
    rhi::Buffer exposure_state_;     ///< float4: exposure, EV100, valid
    rhi::Buffer histogram_;          ///< uint[256]
    rhi::Buffer exposure_constants_; ///< ExposureConstants
    rhi::Pipeline histogram_pipeline_;
    rhi::Pipeline adapt_pipeline_;
    rhi::Bindings histogram_bindings_;
    rhi::Texture histogram_source_; ///< scene colour the histogram bindings point at
    rhi::Bindings adapt_bindings_;
    bool exposure_reset_ = true;
    bool exposure_active_ = false; ///< automatic exposure ran last frame
    float frame_delta_override_ = -1.0f;
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::array<rhi::Pipeline, 4> mesh_pipelines_; ///< by Variant (cull back / front / none, wireframe)
    std::map<std::pair<std::array<void*, 6>, Variant>, CachedBindings> mesh_bindings_;
    rhi::Texture white_;       ///< default for colour, ORM, occlusion, emissive slots
    rhi::Texture flat_normal_; ///< default normal map
    std::unordered_map<std::string, rhi::Texture> path_textures_; ///< model images by path ("" = failed)
    std::uint64_t draw_frame_ = 0;
    View view_;
    ResidencyManager* residency_ = nullptr;
    rhi::Buffer object_constants_;
    rhi::Buffer material_constants_;
    scene::World* world_ = nullptr;
    assets::MeshLibrary* meshes_ = nullptr;
    std::unordered_map<std::string, GpuMesh> gpu_meshes_;
    std::uint32_t last_mesh_draws_ = 0;
    std::vector<MeshDraw> draws_;                     ///< collect_draws() scratch
    std::deque<scene::MeshRenderer> scatter_meshes_; ///< a renderer per scatter mesh (stable addresses)
    std::unordered_map<std::string, std::uint64_t> spline_meshes_; ///< ribbon mesh path -> geometry revision
    rhi::Buffer line_vertices_;
    std::uint64_t line_capacity_ = 0; // vertices
    std::uint32_t last_line_vertices_ = 0;
    bool debug_lines_ = true;

    // Clustered lighting.
    rhi::Pipeline cluster_pipeline_;
    rhi::Bindings cluster_bindings_;
    rhi::Buffer lights_; ///< GpuLight[]
    std::uint32_t light_capacity_ = 0;
    rhi::Buffer cluster_counts_;  ///< uint[kClusterCount]
    rhi::Buffer cluster_indices_; ///< uint[kClusterCount * kMaxLightsPerCluster]
    std::uint32_t last_local_lights_ = 0;

    // Sky, environment and shadows.
    SkyRenderer sky_;
    SkyConstants sky_constants_{};
    scene::Sky cached_sky_{};
    float cached_sky_height_ = -1.0f;
    bool cached_sky_valid_ = false;
    SkyLighting sky_lighting_{};
    bool env_baked_ = false;
    std::uint64_t env_revision_ = 0;
    FrameLighting frame_;
    ShadowSettings shadow_settings_;
    rhi::Texture shadow_map_; ///< D32 2D array, one slice per cascade
    rhi::Pipeline shadow_pipeline_;
    rhi::Bindings shadow_bindings_;
    rhi::Buffer shadow_pass_constants_;

    // Reflection probes (keyed by entity).
    struct Probe {
        glm::vec3 position{0.0f};
        float radius = 0.0f;
        std::uint32_t resolution = 0;
        std::uint32_t slot = 0;
        rhi::Texture capture;  ///< rendered cube with mips
        rhi::Texture specular; ///< prefiltered
        std::uint64_t env_revision = 0;
        std::uint64_t request = 0;
        bool baked = false;
        bool seen = false;
    };
    std::map<std::uint32_t, Probe> probes_;
    std::uint64_t probe_request_ = 0;
    rhi::Texture probe_depth_;
    bool capturing_probe_ = false; ///< draw with the sky's environment and reversed winding

    // Terrain: one entry per terrain entity (keyed by GUID).
    struct GpuTerrain {
        rhi::Texture heights;
        const void* source = nullptr; ///< the heightfield uploaded (authored or derived surface)
        rhi::Format format = rhi::Format::Unknown;
        std::uint32_t samples = 0;
        std::uint64_t revision = 0;
        rhi::Buffer nodes; ///< uint4 per node: camera selection, then the shadow selection
        std::uint32_t node_capacity = 0;
        std::uint32_t camera_full = 0, camera_quarter = 0; ///< whole nodes, then quarter nodes
        std::uint32_t shadow_full = 0, shadow_quarter = 0; ///< after the camera's
        TerrainConstants constants{};
        rhi::Texture splat;                 ///< layer weights (Tex2DArray RGBA8), or the 1x1 default
        std::uint32_t splat_resolution = 0;
        std::uint32_t splat_maps = 0;       ///< 0: the default (layer 0 everywhere)
        std::uint64_t splat_revision = 0;
        rhi::Texture layer_albedo;          ///< Tex2DArray RGBA8: sRGB colour, A height (one slice per layer)
        rhi::Texture layer_normal;          ///< Tex2DArray RGBA8: normal XY, roughness, AO
        std::uint64_t layer_key = 0;        ///< hash of the textures they were built from
        std::array<std::uint32_t, 16> layer_flags{}; ///< kLayer* per layer
        std::array<rhi::Bindings, 4> bindings;    ///< by TerrainPass
        std::array<const void*, 7> binding_key{}; ///< the shared resources they point at
        std::uint64_t last_frame = 0;
        bool seen = false;
    };
    std::unordered_map<core::Guid, GpuTerrain> terrains_;
    std::array<rhi::Pipeline, 4> terrain_pipelines_; ///< by TerrainPass
    rhi::Buffer terrain_grid_vertices_;
    rhi::Buffer terrain_grid_indices_;
    std::uint32_t terrain_full_indices_ = 0;
    std::uint32_t terrain_quarter_indices_ = 0;
    rhi::Buffer terrain_constants_;
    rhi::Buffer terrain_layer_constants_;
    std::uint32_t last_terrain_nodes_ = 0;

    // Water: a GPU mesh per lake surface / river strip (re-uploaded when it changes),
    // the ocean grid, and this frame's draws.
    struct GpuWaterMesh {
        std::weak_ptr<const void> source;
        rhi::Buffer positions; ///< float3 world
        rhi::Buffer flow;      ///< float2
        rhi::Buffer indices;   ///< u32
        std::uint32_t index_count = 0;
        std::uint64_t last_frame = 0;
    };
    struct WaterDraw {
        const GpuWaterMesh* mesh = nullptr;
        WaterConstants constants{};
    };
    std::unordered_map<std::string, GpuWaterMesh> water_meshes_;
    GpuWaterMesh ocean_grid_;
    std::vector<WaterDraw> water_draws_;
    rhi::Pipeline water_pipeline_;
    rhi::Pipeline water_copy_pipeline_;
    rhi::Buffer water_constants_;
    rhi::Texture water_detail_;
    rhi::Bindings water_bindings_;
    rhi::Bindings water_copy_bindings_;
    std::array<const void*, 4> water_binding_key_{};
    std::array<const void*, 2> water_copy_key_{};
    float water_time_ = 0.0f;
    bool water_detail_needs_upload_ = false;
    /// Layer images resampled for the texture arrays, by reference key and size (empty: failed).
    std::unordered_map<std::string, std::vector<std::uint8_t>> layer_images_;

    graph::TransientPool transients_;
    std::unique_ptr<graph::RenderGraph> last_graph_;

    rhi::Pipeline pick_pipeline_;
    rhi::Bindings pick_bindings_;
    rhi::Texture pick_target_;
};

} // namespace f3d::render::raster
