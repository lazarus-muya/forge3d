// SPDX-License-Identifier: Apache-2.0
#pragma once

// Terrain (phases.md P6.1): a heightfield made of 256 x 256-quad tiles, rendered
// with CDLOD (Strugar, "Continuous Distance-Dependent Level of Detail", 2009).
//
// A Terrain component holds the parameters; the heights live in the World's
// HeightfieldStore (registry context), keyed by the entity's GUID: they are far too
// big for a component (an 8k terrain is 64M samples). Terrain-local space: x and z
// in [-size/2, size/2], y the height in metres; the entity's world translation
// places it (terrains don't rotate or scale).

#include "f3d/core/asset_ref.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"

#include <entt/entity/fwd.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::terrain {

/// Quads per tile side (a tile has kTileQuads + 1 samples per side, shared with its neighbours).
inline constexpr std::uint32_t kTileQuads = 256;
/// Quads per side of the instanced CDLOD grid (and of a finest-LOD node).
inline constexpr std::uint32_t kGridQuads = 32;
inline constexpr std::uint32_t kMaxLods = 12;
/// Material layers (P6.4): at most 16, weighted by splat maps of 4 layers each.
inline constexpr std::uint32_t kMaxLayers = 16;
/// Splat texels per tile side (a texel covers 2 x 2 quads).
inline constexpr std::uint32_t kSplatPerTile = 128;

enum class HeightFormat : std::uint8_t {
    R16,  ///< 16-bit normalised over [min_height, max_height] (GPU and files)
    R32F, ///< 32-bit float metres
};

/// Where a layer covers the terrain when the rules are applied (terrain.apply_rules):
/// inside both ranges, fading out over `falloff` (metres for the height, degrees for
/// the slope) at each end.
struct LayerRule {
    bool enabled = false;
    float height_min = -1.0e4f; ///< metres (terrain-local)
    float height_max = 1.0e4f;
    float slope_min = 0.0f;     ///< degrees
    float slope_max = 90.0f;
    float falloff = 4.0f;
};

/// One material layer. Its surface comes from its own textures (any image: a project
/// texture, an image file, or a built-in "builtin:terrain/..." one), else from its
/// material's textures, else from `color` and `roughness` with procedural detail.
/// Without an albedo texture, `color` is the albedo; with one, it tints it (white:
/// unchanged).
struct TerrainLayer {
    std::string name = "Layer";
    core::AssetRef material;              ///< optional: its base colour, roughness, metallic and textures
    glm::vec3 color{0.25f, 0.3f, 0.15f};  ///< linear albedo, or the tint of the albedo texture
    float roughness = 0.9f;               ///< without a roughness texture
    float tiling = 4.0f;    ///< metres per repeat of the layer's textures
    float anti_tiling = 0.5f; ///< 0: plain repeats; up to 1: more of the ground uses shifted copies, hiding the grid
    bool triplanar = false; ///< project the textures on all three axes (cliffs)
    core::AssetRef albedo_texture;    ///< colour (sRGB)
    core::AssetRef normal_texture;    ///< tangent-space normal map
    core::AssetRef roughness_texture; ///< grey: roughness
    core::AssetRef ao_texture;        ///< grey: ambient occlusion
    core::AssetRef height_texture;    ///< grey: height, for blending (default: the albedo's brightness)
    bool normal_directx = false;      ///< the normal map is DirectX style (green down)
    float normal_strength = 1.0f;
    float height_contrast = 1.0f; ///< sharpness of the borders with other layers (0: soft, 2: crisp)
    LayerRule rule;
};

/// Texture-set slots of a layer (TerrainLayer's *_texture fields).
enum class LayerTextureSlot : std::uint8_t { Albedo, Normal, Roughness, Ao, Height };
inline constexpr std::uint32_t kLayerTextureSlots = 5;
[[nodiscard]] core::AssetRef& layer_texture(TerrainLayer& layer, LayerTextureSlot slot) noexcept;
[[nodiscard]] const core::AssetRef& layer_texture(const TerrainLayer& layer, LayerTextureSlot slot) noexcept;

/// Which slot an image file belongs to, from its name ("rock_Normal.png" -> Normal,
/// "*_diff*", "*albedo*", "*color*", "*basecolor*" -> Albedo, "*rough*" -> Roughness,
/// "*_ao*", "*occlusion*" -> Ao, "*height*", "*disp*" -> Height), or nullopt.
[[nodiscard]] std::optional<LayerTextureSlot> guess_texture_slot(std::string_view path);
/// The texture set an image belongs to: the image itself in its slot, and the
/// other images in `siblings` (paths in the same folder) whose name matches it
/// apart from the slot word ("Rock_Color.jpg" -> "Rock_NormalGL.jpg", "Rock_Roughness.jpg").
[[nodiscard]] std::array<std::string, kLayerTextureSlots> guess_texture_set(std::string_view path,
                                                                             const std::vector<std::string>& siblings);
/// A packed occlusion/roughness/metal map ("*_arm_*", "*_orm_*": R occlusion, G
/// roughness, B metal), as Poly Haven ships; guess_texture_set puts it in the
/// roughness and AO slots when the set has no separate maps for them.
[[nodiscard]] bool is_packed_orm(std::string_view path);
/// A layer name for a texture set from one of its file names, without the slot word
/// and resolution ("aerial_rocks_02_diff_2k.jpg" -> "Aerial rocks 02").
[[nodiscard]] std::string texture_set_name(std::string_view path);

/// A terrain's parameters (the heights are in the HeightfieldStore).
struct Terrain {
    std::uint32_t tiles = 4;   ///< per side: samples per side = tiles * 256 + 1
    float size = 1024.0f;      ///< metres per side
    float min_height = 0.0f;   ///< metres
    float max_height = 256.0f; ///< metres
    HeightFormat format = HeightFormat::R16;
    float lod_distance = 2.0f; ///< finest LOD range, in finest-node sizes (detail vs speed)
    std::vector<TerrainLayer> layers; ///< bottom to top; empty: one default layer
    std::uint32_t texture_resolution = 1024; ///< texels per side of every layer texture on the GPU

    [[nodiscard]] std::uint32_t samples() const noexcept { return tiles * kTileQuads + 1; }
    [[nodiscard]] float spacing() const noexcept { return size / static_cast<float>(samples() - 1); }
};

/// Grass on flats, rock on slopes over 35 degrees, snow on the top 15 % (with rules).
[[nodiscard]] std::vector<TerrainLayer> default_layers(float min_height, float max_height);

/// Layer weights per splat texel: up to 16 bytes (4 RGBA8 maps), summing to 255; all
/// zero means layer 0. Tiles of kSplatPerTile texels are marked dirty for upload.
class Splatmap {
public:
    Splatmap(std::uint32_t tiles, std::uint32_t maps);

    [[nodiscard]] std::uint32_t tiles() const noexcept { return tiles_; }
    [[nodiscard]] std::uint32_t resolution() const noexcept { return tiles_ * kSplatPerTile; }
    [[nodiscard]] std::uint32_t maps() const noexcept { return maps_; }
    /// Adds maps (zero weights) until `maps` (1..4) exist.
    void ensure_maps(std::uint32_t maps);

    [[nodiscard]] std::uint8_t weight(std::uint32_t x, std::uint32_t z, std::uint32_t layer) const noexcept;
    void set_weight(std::uint32_t x, std::uint32_t z, std::uint32_t layer, std::uint8_t value) noexcept;
    /// One map's RGBA8 texels, row-major.
    [[nodiscard]] std::uint8_t* map(std::uint32_t index) noexcept { return data_.data() + index * map_bytes(); }
    [[nodiscard]] const std::uint8_t* map(std::uint32_t index) const noexcept {
        return data_.data() + index * map_bytes();
    }
    [[nodiscard]] std::size_t map_bytes() const noexcept {
        return static_cast<std::size_t>(resolution()) * resolution() * 4;
    }

    void mark_dirty(std::uint32_t tile) noexcept;
    void mark_all_dirty() noexcept;
    [[nodiscard]] std::vector<std::uint32_t> take_dirty_tiles();
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    /// The revision at which a tile last changed (for consumers other than the renderer).
    [[nodiscard]] std::uint64_t tile_revision(std::uint32_t tile) const noexcept {
        return tile < tile_revisions_.size() ? tile_revisions_[tile] : revision_;
    }

private:
    std::uint32_t tiles_;
    std::uint32_t maps_;
    std::vector<std::uint8_t> data_; ///< maps_ maps
    std::vector<bool> dirty_tiles_;
    std::vector<std::uint64_t> tile_revisions_;
    std::uint64_t revision_ = 1;
};

/// Registers Terrain and HeightFormat with reflection (and Terrain as a scene
/// component type). Idempotent.
void register_types();

/// Heights in metres, row-major (z rows, x columns), samples() x samples().
class Heightfield {
public:
    Heightfield(std::uint32_t tiles, float size);

    [[nodiscard]] std::uint32_t tiles() const noexcept { return tiles_; }
    [[nodiscard]] std::uint32_t samples() const noexcept { return samples_; }
    [[nodiscard]] float size() const noexcept { return size_; }
    [[nodiscard]] float spacing() const noexcept { return size_ / static_cast<float>(samples_ - 1); }

    [[nodiscard]] float at(std::uint32_t x, std::uint32_t z) const noexcept { return heights_[index(x, z)]; }
    void set(std::uint32_t x, std::uint32_t z, float h) noexcept { heights_[index(x, z)] = h; }
    [[nodiscard]] const std::vector<float>& heights() const noexcept { return heights_; }
    [[nodiscard]] std::vector<float>& heights() noexcept { return heights_; }

    /// Bilinear height at terrain-local (x, z) metres (clamped to the edges).
    [[nodiscard]] float height_at(float x, float z) const noexcept;
    /// Unit normal at terrain-local (x, z) from central differences.
    [[nodiscard]] glm::vec3 normal_at(float x, float z) const noexcept;

    /// Recomputes the min/max tree (call after changing heights; `dirty` limits it to a
    /// sample rectangle, x0..x1 and z0..z1 inclusive).
    void update_bounds();
    void update_bounds(std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1);
    /// Height range of the node of `quads` x `quads` quads at sample (x, z) (a node of the
    /// CDLOD quadtree: quads is kGridQuads << lod, x and z multiples of it).
    [[nodiscard]] glm::vec2 node_range(std::uint32_t x, std::uint32_t z, std::uint32_t quads) const noexcept;
    [[nodiscard]] glm::vec2 range() const noexcept; ///< min and max of all heights

    /// Marks tiles changed (renderer uploads them); x0..x1, z0..z1 are samples.
    void mark_dirty(std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1);
    void mark_all_dirty();
    /// Tiles changed since the last call (and clears the list); tile index = tz * tiles + tx.
    [[nodiscard]] std::vector<std::uint32_t> take_dirty_tiles();
    /// Bumped by every change: renderers compare it to know they're current.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    /// The revision at which a tile last changed (consumers other than the renderer,
    /// which takes the dirty list, compare these).
    [[nodiscard]] std::uint64_t tile_revision(std::uint32_t tile) const noexcept {
        return tile < tile_revisions_.size() ? tile_revisions_[tile] : revision_;
    }

    /// The layer weights, or null before any were painted or baked (then layer 0 covers
    /// everything).
    [[nodiscard]] Splatmap* splat() noexcept { return splat_.get(); }
    [[nodiscard]] const Splatmap* splat() const noexcept { return splat_.get(); }
    /// The splat maps, created (layer 0 everywhere) or grown to hold `layers` layers.
    Splatmap& ensure_splat(std::uint32_t layers);

private:
    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t z) const noexcept {
        return static_cast<std::size_t>(z) * samples_ + x;
    }

    std::uint32_t tiles_;
    std::uint32_t samples_;
    float size_;
    std::vector<float> heights_;
    /// Min/max per finest node (kGridQuads), then each coarser level: levels_[l] has
    /// (nodes >> l)^2 entries.
    std::vector<std::vector<glm::vec2>> levels_;
    std::vector<bool> dirty_tiles_;
    std::vector<std::uint64_t> tile_revisions_;
    std::uint64_t revision_ = 1;
    std::unique_ptr<Splatmap> splat_;
};

/// FNV-1a 64 of the heights' bytes, as 16 hex digits (determinism checks).
[[nodiscard]] std::string height_hash(const Heightfield& heightfield);

/// The heightfields of a World, keyed by terrain entity GUID. Each terrain has its
/// authored heightfield (what sculpting, erosion, painting and saving work on) and,
/// while splines change it, a derived surface (the authored data with the splines'
/// roads, riverbeds and paint applied: world::update_landscape). Rendering, raycasts,
/// scatter and water stand on the surface.
class HeightfieldStore {
public:
    [[nodiscard]] std::shared_ptr<Heightfield> find(const core::Guid& terrain) const;
    void set(const core::Guid& terrain, std::shared_ptr<Heightfield> heightfield);
    void erase(const core::Guid& terrain);
    /// The derived surface, or null when the terrain has none (the authored one is the surface).
    [[nodiscard]] std::shared_ptr<Heightfield> find_surface(const core::Guid& terrain) const;
    void set_surface(const core::Guid& terrain, std::shared_ptr<Heightfield> surface);
    void erase_surface(const core::Guid& terrain);

private:
    std::unordered_map<core::Guid, std::shared_ptr<Heightfield>> map_;
    std::unordered_map<core::Guid, std::shared_ptr<Heightfield>> surfaces_;
};

/// The World's store (created on first use).
[[nodiscard]] HeightfieldStore& heightfields(scene::World& world);
/// The authored heightfield of a terrain entity, or null.
[[nodiscard]] std::shared_ptr<Heightfield> heightfield_of(scene::World& world, entt::entity terrain);
/// What the terrain looks like: its derived surface if it has one, else the authored heightfield.
[[nodiscard]] std::shared_ptr<Heightfield> surface_of(scene::World& world, entt::entity terrain);

/// fBm noise parameters for generated terrains.
struct NoiseSettings {
    std::uint32_t seed = 1;
    float frequency = 4.0f; ///< features across the terrain at the first octave
    std::uint32_t octaves = 7;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
};

/// 2D gradient noise in about [-0.7, 0.7]; deterministic for a seed.
[[nodiscard]] float gradient_noise(float x, float y, std::uint32_t seed) noexcept;

/// Fills `heightfield` with fBm gradient noise mapped to [min_height, max_height].
/// Deterministic for a seed (any thread count).
void generate_noise(Heightfield& heightfield, const NoiseSettings& noise, float min_height, float max_height);

// --- CDLOD -----------------------------------------------------------------------------

/// A selected quadtree node: draw the grid over it at a LOD.
struct LodNode {
    std::uint32_t x = 0; ///< first sample
    std::uint32_t z = 0;
    std::uint32_t lod = 0;   ///< grid step = 1 << lod samples
    std::uint32_t quads = 0; ///< covered: kGridQuads << lod (a whole node), or half that (one quarter)
};

struct LodSelection {
    std::vector<LodNode> nodes;
    std::array<float, kMaxLods> ranges{}; ///< metres: LOD l is used up to ranges[l]
    std::uint32_t lod_count = 0;
};

/// Number of LODs for a terrain (the root node covers it whole).
[[nodiscard]] std::uint32_t lod_count(const Heightfield& heightfield) noexcept;

/// Selects the nodes to draw for a camera at terrain-local `camera`. `frustum`
/// (optional): 6 planes (xyz normal pointing in, w distance) in terrain-local space;
/// nodes entirely outside are skipped.
[[nodiscard]] LodSelection select_lods(const Heightfield& heightfield, const Terrain& terrain, const glm::vec3& camera,
                                       const std::array<glm::vec4, 6>* frustum = nullptr);

/// The frustum planes of a view-projection matrix (points satisfying dot(n, p) + w >= 0 are inside).
[[nodiscard]] std::array<glm::vec4, 6> frustum_planes(const glm::mat4& view_projection) noexcept;

// --- Raycast ---------------------------------------------------------------------------

struct TerrainHit {
    entt::entity entity;
    glm::vec3 point{0.0f};  ///< world
    glm::vec3 normal{0.0f}; ///< world
    float distance = 0.0f;
};

/// The first hit of a terrain-local ray (unit direction), or nullopt.
[[nodiscard]] std::optional<glm::vec3> raycast(const Heightfield& heightfield, const glm::vec3& origin,
                                               const glm::vec3& direction, float max_distance = 1.0e6f);

/// The nearest visible terrain hit of a world ray, or nullopt.
[[nodiscard]] std::optional<TerrainHit> raycast(scene::World& world, const glm::vec3& origin,
                                                const glm::vec3& direction);

/// The terrain's world origin (its entity's world translation).
[[nodiscard]] glm::vec3 terrain_origin(scene::World& world, entt::entity terrain);

} // namespace f3d::terrain
