// SPDX-License-Identifier: Apache-2.0
#include "f3d/terrain/terrain.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace f3d::terrain {

void register_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        entt::meta_factory<HeightFormat>{}
            .type("HeightFormat")
            .data<HeightFormat::R16>("R16")
            .data<HeightFormat::R32F>("R32F");
        entt::meta_factory<LayerRule>{}
            .type("LayerRule")
            .data<&LayerRule::enabled>("enabled")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Paint this layer automatically by height and slope"))
            .data<&LayerRule::height_min>("height_min")
            .custom<FieldMeta>(FieldMeta{}.with_label("Lowest height").with_unit("m").with_tooltip(
                "The layer covers ground at least this high"))
            .data<&LayerRule::height_max>("height_max")
            .custom<FieldMeta>(FieldMeta{}.with_label("Highest height").with_unit("m").with_tooltip(
                "The layer covers ground at most this high"))
            .data<&LayerRule::slope_min>("slope_min")
            .custom<FieldMeta>(FieldMeta{}.with_label("Least slope").with_unit("deg").with_range(0.0, 90.0, 0.5)
                                   .with_tooltip("The layer covers ground at least this steep (e.g. 35: rock on cliffs)"))
            .data<&LayerRule::slope_max>("slope_max")
            .custom<FieldMeta>(FieldMeta{}.with_label("Steepest slope").with_unit("deg").with_range(0.0, 90.0, 0.5)
                                   .with_tooltip("The layer covers ground at most this steep"))
            .data<&LayerRule::falloff>("falloff")
            .custom<FieldMeta>(FieldMeta{}.with_label("Blend").with_tooltip(
                "Width of the soft edge to the next layer: metres (height), degrees (slope)"));
        entt::meta_factory<TerrainLayer>{}
            .type("TerrainLayer")
            .data<&TerrainLayer::name>("name")
            .data<&TerrainLayer::material>("material")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Material painted by this layer"))
            .data<&TerrainLayer::color>("color")
            .custom<FieldMeta>(FieldMeta{}.with_label("Colour").with_color().with_tooltip("Tint (the colour itself without a material)"))
            .data<&TerrainLayer::roughness>("roughness")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01).with_tooltip("0: glossy, 1: matte"))
            .data<&TerrainLayer::tiling>("tiling")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 1000.0, 0.1).with_tooltip(
                "Size of one texture repeat on the ground"))
            .data<&TerrainLayer::anti_tiling>("anti_tiling")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01).with_tooltip(
                "Hides the repeating grid: blends shifted copies of the textures across the ground (0: off)"))
            .data<&TerrainLayer::triplanar>("triplanar")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Project the texture from three sides: no stretching on cliffs"))
            .data<&TerrainLayer::albedo_texture>("albedo_texture")
            .custom<FieldMeta>(FieldMeta{}.with_label("Colour map").with_tooltip("Colour texture (any image)"))
            .data<&TerrainLayer::normal_texture>("normal_texture")
            .custom<FieldMeta>(FieldMeta{}.with_label("Normal map"))
            .data<&TerrainLayer::roughness_texture>("roughness_texture")
            .custom<FieldMeta>(FieldMeta{}.with_label("Roughness map"))
            .data<&TerrainLayer::ao_texture>("ao_texture")
            .custom<FieldMeta>(FieldMeta{}.with_label("AO map").with_tooltip("Ambient occlusion"))
            .data<&TerrainLayer::height_texture>("height_texture")
            .custom<FieldMeta>(FieldMeta{}.with_label("Height map").with_tooltip(
                "Shapes the borders with other layers (default: the colour map's brightness)"))
            .data<&TerrainLayer::normal_directx>("normal_directx")
            .custom<FieldMeta>(FieldMeta{}.with_label("DirectX normals").with_tooltip(
                "The normal map has green pointing down (DirectX convention)"))
            .data<&TerrainLayer::normal_strength>("normal_strength")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 4.0, 0.01))
            .data<&TerrainLayer::height_contrast>("height_contrast")
            .custom<FieldMeta>(FieldMeta{}.with_label("Border sharpness").with_range(0.0, 4.0, 0.01).with_tooltip(
                "How crisply this layer meets the others, following its height map"))
            .data<&TerrainLayer::rule>("rule")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Where the layer appears automatically"));
        entt::meta_factory<Terrain>{}
            .type("Terrain")
            .data<&Terrain::tiles>("tiles")
            .custom<FieldMeta>(FieldMeta{}.with_read_only().with_tooltip(
                "Resolution: 256 x 256 quads per tile, tiles per side (set when created)"))
            .data<&Terrain::size>("size")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_read_only().with_tooltip(
                "Width and depth of the terrain (set when created)"))
            .data<&Terrain::min_height>("min_height")
            .custom<FieldMeta>(FieldMeta{}.with_label("Lowest height").with_unit("m").with_tooltip(
                "The lowest the ground can go"))
            .data<&Terrain::max_height>("max_height")
            .custom<FieldMeta>(FieldMeta{}.with_label("Highest height").with_unit("m").with_tooltip(
                "The highest the ground can go"))
            .data<&Terrain::format>("format")
            .custom<FieldMeta>(FieldMeta{}.with_label("Precision").with_tooltip(
                "Height storage: R16 (16-bit, compact) or R32F (32-bit float, finer steps)"))
            .data<&Terrain::lod_distance>("lod_distance")
            .custom<FieldMeta>(FieldMeta{}.with_label("Detail distance").with_range(1.0, 8.0, 0.1).with_tooltip(
                "How far full detail reaches: higher looks better further away but is slower"))
            .data<&Terrain::layers>("layers")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Material layers, bottom to top (Landscape panel)"))
            .data<&Terrain::texture_resolution>("texture_resolution")
            .custom<FieldMeta>(FieldMeta{}.with_label("Texture resolution").with_range(128.0, 4096.0, 1.0).with_tooltip(
                "Texels per side of each layer texture on the GPU (memory: 8 bytes per texel per layer)"));
        // Created by terrain.create (it needs heights), not by entity.add_component.
        scene::register_component<Terrain>("Terrain", false);
        return true;
    }();
    static_cast<void>(registered);
}

Heightfield::Heightfield(std::uint32_t tiles, float size)
    : tiles_(std::max(tiles, 1u)), samples_(tiles_ * kTileQuads + 1), size_(size),
      heights_(static_cast<std::size_t>(samples_) * samples_, 0.0f),
      dirty_tiles_(static_cast<std::size_t>(tiles_) * tiles_, true),
      tile_revisions_(static_cast<std::size_t>(tiles_) * tiles_, 1) {
    update_bounds();
}

float Heightfield::height_at(float x, float z) const noexcept {
    const float s = static_cast<float>(samples_ - 1);
    const float fx = std::clamp((x / size_ + 0.5f) * s, 0.0f, s);
    const float fz = std::clamp((z / size_ + 0.5f) * s, 0.0f, s);
    const auto x0 = std::min(static_cast<std::uint32_t>(fx), samples_ - 2);
    const auto z0 = std::min(static_cast<std::uint32_t>(fz), samples_ - 2);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);
    const float a = at(x0, z0) + (at(x0 + 1, z0) - at(x0, z0)) * tx;
    const float b = at(x0, z0 + 1) + (at(x0 + 1, z0 + 1) - at(x0, z0 + 1)) * tx;
    return a + (b - a) * tz;
}

glm::vec3 Heightfield::normal_at(float x, float z) const noexcept {
    const float d = spacing();
    const float hx = height_at(x + d, z) - height_at(x - d, z);
    const float hz = height_at(x, z + d) - height_at(x, z - d);
    return glm::normalize(glm::vec3(-hx, 2.0f * d, -hz));
}

void Heightfield::update_bounds() {
    update_bounds(0, 0, samples_ - 1, samples_ - 1);
}

void Heightfield::update_bounds(std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1) {
    F3D_ZONE;
    const std::uint32_t nodes = (samples_ - 1) / kGridQuads;
    std::uint32_t level_count = 1;
    while ((nodes >> (level_count - 1)) > 1) {
        ++level_count;
    }
    if (levels_.size() != level_count) {
        levels_.assign(level_count, {});
        for (std::uint32_t l = 0; l < level_count; ++l) {
            const std::uint32_t n = std::max(nodes >> l, 1u);
            levels_[l].assign(static_cast<std::size_t>(n) * n, glm::vec2(0.0f));
        }
        x0 = z0 = 0;
        x1 = z1 = samples_ - 1;
    }
    // Finest level from samples (a node includes its shared edge samples).
    const std::uint32_t nx0 = std::min(x0 / kGridQuads, nodes - 1);
    const std::uint32_t nz0 = std::min(z0 / kGridQuads, nodes - 1);
    const std::uint32_t nx1 = std::min(x1 / kGridQuads, nodes - 1);
    const std::uint32_t nz1 = std::min(z1 / kGridQuads, nodes - 1);
    for (std::uint32_t nz = (nz0 > 0 ? nz0 - 1 : 0); nz <= nz1; ++nz) {
        for (std::uint32_t nx = (nx0 > 0 ? nx0 - 1 : 0); nx <= nx1; ++nx) {
            float lo = 1e30f;
            float hi = -1e30f;
            for (std::uint32_t z = nz * kGridQuads; z <= (nz + 1) * kGridQuads; ++z) {
                for (std::uint32_t x = nx * kGridQuads; x <= (nx + 1) * kGridQuads; ++x) {
                    const float h = at(x, z);
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
            }
            levels_[0][static_cast<std::size_t>(nz) * nodes + nx] = {lo, hi};
        }
    }
    for (std::uint32_t l = 1; l < level_count; ++l) {
        const std::uint32_t n = std::max(nodes >> l, 1u);
        const std::uint32_t child = nodes >> (l - 1);
        for (std::uint32_t z = (nz0 >> l); z <= std::min(nz1 >> l, n - 1); ++z) {
            for (std::uint32_t x = (nx0 >> l); x <= std::min(nx1 >> l, n - 1); ++x) {
                glm::vec2 r(1e30f, -1e30f);
                for (std::uint32_t c = 0; c < 4; ++c) {
                    const std::uint32_t cx = std::min(x * 2 + (c & 1), child - 1);
                    const std::uint32_t cz = std::min(z * 2 + (c >> 1), child - 1);
                    const glm::vec2 cr = levels_[l - 1][static_cast<std::size_t>(cz) * child + cx];
                    r = {std::min(r.x, cr.x), std::max(r.y, cr.y)};
                }
                levels_[l][static_cast<std::size_t>(z) * n + x] = r;
            }
        }
    }
    ++revision_;
}

glm::vec2 Heightfield::node_range(std::uint32_t x, std::uint32_t z, std::uint32_t quads) const noexcept {
    std::uint32_t level = 0;
    while ((kGridQuads << level) < quads && level + 1 < levels_.size()) {
        ++level;
    }
    const std::uint32_t nodes = (samples_ - 1) / kGridQuads;
    const std::uint32_t n = std::max(nodes >> level, 1u);
    const std::uint32_t nx = std::min(x / (kGridQuads << level), n - 1);
    const std::uint32_t nz = std::min(z / (kGridQuads << level), n - 1);
    return levels_[level][static_cast<std::size_t>(nz) * n + nx];
}

glm::vec2 Heightfield::range() const noexcept {
    return levels_.back().front();
}

void Heightfield::mark_dirty(std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1) {
    // A sample on a tile edge belongs to both tiles.
    const auto first = [](std::uint32_t s) { return s == 0 ? 0u : (s - 1) / kTileQuads; };
    const auto last = [this](std::uint32_t s) { return std::min(s / kTileQuads, tiles_ - 1); };
    for (std::uint32_t tz = first(z0); tz <= last(z1); ++tz) {
        for (std::uint32_t tx = first(x0); tx <= last(x1); ++tx) {
            dirty_tiles_[static_cast<std::size_t>(tz) * tiles_ + tx] = true;
            tile_revisions_[static_cast<std::size_t>(tz) * tiles_ + tx] = revision_ + 1;
        }
    }
    ++revision_;
}

void Heightfield::mark_all_dirty() {
    std::fill(dirty_tiles_.begin(), dirty_tiles_.end(), true);
    ++revision_;
    std::fill(tile_revisions_.begin(), tile_revisions_.end(), revision_);
}

std::vector<std::uint32_t> Heightfield::take_dirty_tiles() {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < dirty_tiles_.size(); ++i) {
        if (dirty_tiles_[i]) {
            out.push_back(i);
            dirty_tiles_[i] = false;
        }
    }
    return out;
}

std::string height_hash(const Heightfield& heightfield) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(heightfield.heights().data());
    const std::size_t size = heightfield.heights().size() * sizeof(float);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ bytes[i]) * 0x100000001b3ull;
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(h));
    return text;
}

std::shared_ptr<Heightfield> HeightfieldStore::find(const core::Guid& terrain) const {
    const auto it = map_.find(terrain);
    return it == map_.end() ? nullptr : it->second;
}

void HeightfieldStore::set(const core::Guid& terrain, std::shared_ptr<Heightfield> heightfield) {
    map_[terrain] = std::move(heightfield);
}

void HeightfieldStore::erase(const core::Guid& terrain) {
    map_.erase(terrain);
    surfaces_.erase(terrain);
}

std::shared_ptr<Heightfield> HeightfieldStore::find_surface(const core::Guid& terrain) const {
    const auto it = surfaces_.find(terrain);
    return it == surfaces_.end() ? nullptr : it->second;
}

void HeightfieldStore::set_surface(const core::Guid& terrain, std::shared_ptr<Heightfield> surface) {
    surfaces_[terrain] = std::move(surface);
}

void HeightfieldStore::erase_surface(const core::Guid& terrain) {
    surfaces_.erase(terrain);
}

HeightfieldStore& heightfields(scene::World& world) {
    auto& ctx = world.registry().ctx();
    if (!ctx.contains<HeightfieldStore>()) {
        ctx.emplace<HeightfieldStore>();
    }
    return ctx.get<HeightfieldStore>();
}

std::shared_ptr<Heightfield> heightfield_of(scene::World& world, entt::entity terrain) {
    if (terrain == entt::null || !world.registry().all_of<Terrain>(terrain)) {
        return nullptr;
    }
    return heightfields(world).find(world.guid(terrain));
}

std::shared_ptr<Heightfield> surface_of(scene::World& world, entt::entity terrain) {
    if (terrain == entt::null || !world.registry().all_of<Terrain>(terrain)) {
        return nullptr;
    }
    const HeightfieldStore& store = heightfields(world);
    const core::Guid guid = world.guid(terrain);
    if (auto surface = store.find_surface(guid)) {
        return surface;
    }
    return store.find(guid);
}

// --- Noise -----------------------------------------------------------------------------------

namespace {

std::uint32_t hash(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept {
    std::uint32_t h =
        static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

} // namespace

float gradient_noise(float x, float y, std::uint32_t seed) noexcept {
    const auto ix = static_cast<std::int32_t>(std::floor(x));
    const auto iy = static_cast<std::int32_t>(std::floor(y));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const auto grad = [&](std::int32_t cx, std::int32_t cy, float dx, float dy) {
        const float angle = static_cast<float>(hash(cx, cy, seed) & 0xFFFFu) * (6.2831853f / 65536.0f);
        return std::cos(angle) * dx + std::sin(angle) * dy;
    };
    const float n00 = grad(ix, iy, fx, fy);
    const float n10 = grad(ix + 1, iy, fx - 1.0f, fy);
    const float n01 = grad(ix, iy + 1, fx, fy - 1.0f);
    const float n11 = grad(ix + 1, iy + 1, fx - 1.0f, fy - 1.0f);
    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const float a = n00 + (n10 - n00) * ux;
    const float b = n01 + (n11 - n01) * ux;
    return a + (b - a) * uy;
}

void generate_noise(Heightfield& heightfield, const NoiseSettings& noise, float min_height, float max_height) {
    F3D_ZONE;
    const std::uint32_t n = heightfield.samples();
    float amplitude_sum = 0.0f;
    for (std::uint32_t o = 0, a = 1; o < noise.octaves; ++o) {
        amplitude_sum += std::pow(noise.persistence, static_cast<float>(o));
        (void)a;
    }
    std::vector<float>& heights = heightfield.heights();
    const auto rows = [&](std::uint32_t z0, std::uint32_t z1) {
        for (std::uint32_t z = z0; z < z1; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(n - 1);
                const float v = static_cast<float>(z) / static_cast<float>(n - 1);
                float frequency = noise.frequency;
                float amplitude = 1.0f;
                float sum = 0.0f;
                for (std::uint32_t o = 0; o < noise.octaves; ++o) {
                    sum += gradient_noise(u * frequency, v * frequency, noise.seed + o * 101u) * amplitude;
                    frequency *= noise.lacunarity;
                    amplitude *= noise.persistence;
                }
                const float t = std::clamp(sum / std::max(amplitude_sum, 1e-6f) * 1.4f * 0.5f + 0.5f, 0.0f, 1.0f);
                heights[static_cast<std::size_t>(z) * n + x] = min_height + (max_height - min_height) * t;
            }
        }
    };
    const std::uint32_t threads = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    std::vector<std::thread> pool;
    const std::uint32_t chunk = (n + threads - 1) / threads;
    for (std::uint32_t t = 0; t < threads; ++t) {
        const std::uint32_t z0 = t * chunk;
        const std::uint32_t z1 = std::min(n, z0 + chunk);
        if (z0 < z1) {
            pool.emplace_back(rows, z0, z1);
        }
    }
    for (std::thread& t : pool) {
        t.join();
    }
    heightfield.update_bounds();
    heightfield.mark_all_dirty();
}

// --- CDLOD -----------------------------------------------------------------------------------

std::uint32_t lod_count(const Heightfield& heightfield) noexcept {
    std::uint32_t count = 1;
    while ((kGridQuads << (count - 1)) < heightfield.samples() - 1 && count < kMaxLods) {
        ++count;
    }
    return count;
}

std::array<glm::vec4, 6> frustum_planes(const glm::mat4& m) noexcept {
    const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    std::array<glm::vec4, 6> planes = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2}; // depth 0..1
    for (glm::vec4& p : planes) {
        p /= glm::length(glm::vec3(p));
    }
    return planes;
}

namespace {

struct Box {
    glm::vec3 min;
    glm::vec3 max;
};

bool outside(const std::array<glm::vec4, 6>& planes, const Box& b) noexcept {
    for (const glm::vec4& p : planes) {
        const glm::vec3 v(p.x >= 0.0f ? b.max.x : b.min.x, p.y >= 0.0f ? b.max.y : b.min.y,
                          p.z >= 0.0f ? b.max.z : b.min.z);
        if (glm::dot(glm::vec3(p), v) + p.w < 0.0f) {
            return true;
        }
    }
    return false;
}

bool intersects_sphere(const Box& b, const glm::vec3& c, float r) noexcept {
    const glm::vec3 q = glm::clamp(c, b.min, b.max);
    return glm::dot(q - c, q - c) <= r * r;
}

struct Selector {
    const Heightfield& hf;
    const std::array<glm::vec4, 6>* frustum;
    glm::vec3 camera;
    LodSelection& out;

    [[nodiscard]] Box box(std::uint32_t x, std::uint32_t z, std::uint32_t quads) const noexcept {
        const float d = hf.spacing();
        const float half = hf.size() * 0.5f;
        const glm::vec2 r = hf.node_range(x, z, quads);
        return {{static_cast<float>(x) * d - half, r.x, static_cast<float>(z) * d - half},
                {static_cast<float>(x + quads) * d - half, r.y, static_cast<float>(z + quads) * d - half}};
    }

    /// Strugar's recursion. Returns false if the node isn't in its LOD's range (the
    /// parent then covers it).
    bool select(std::uint32_t x, std::uint32_t z, std::uint32_t lod) {
        const std::uint32_t quads = kGridQuads << lod;
        if (x >= hf.samples() - 1 || z >= hf.samples() - 1) {
            return true; // beyond the terrain (a partial root)
        }
        const Box b = box(x, z, quads);
        if (!intersects_sphere(b, camera, out.ranges[lod])) {
            return false;
        }
        if (frustum && outside(*frustum, b)) {
            return true; // handled: nothing to draw
        }
        if (lod == 0 || !intersects_sphere(b, camera, out.ranges[lod - 1])) {
            out.nodes.push_back({x, z, lod, quads});
            return true;
        }
        const std::uint32_t half = quads / 2;
        for (std::uint32_t c = 0; c < 4; ++c) {
            const std::uint32_t cx = x + (c & 1) * half;
            const std::uint32_t cz = z + (c >> 1) * half;
            if (cx >= hf.samples() - 1 || cz >= hf.samples() - 1) {
                continue;
            }
            if (!select(cx, cz, lod - 1)) {
                // The child is beyond its own range: this LOD's grid covers that quarter.
                const Box cb = box(cx, cz, half);
                if (!frustum || !outside(*frustum, cb)) {
                    out.nodes.push_back({cx, cz, lod, half});
                }
            }
        }
        return true;
    }
};

} // namespace

LodSelection select_lods(const Heightfield& heightfield, const Terrain& terrain, const glm::vec3& camera,
                         const std::array<glm::vec4, 6>* frustum) {
    F3D_ZONE;
    LodSelection out;
    out.lod_count = lod_count(heightfield);
    const float leaf = static_cast<float>(kGridQuads) * heightfield.spacing();
    float range = leaf * std::max(terrain.lod_distance, 1.0f);
    for (std::uint32_t l = 0; l < out.lod_count; ++l) {
        out.ranges[l] = range;
        range *= 2.0f;
    }
    // The root's range covers the whole terrain from anywhere.
    out.ranges[out.lod_count - 1] = 1.0e30f;
    Selector selector{heightfield, frustum, camera, out};
    selector.select(0, 0, out.lod_count - 1);
    return out;
}

// --- Raycast ---------------------------------------------------------------------------------

std::optional<glm::vec3> raycast(const Heightfield& hf, const glm::vec3& origin, const glm::vec3& direction,
                                 float max_distance) {
    const float half = hf.size() * 0.5f;
    const glm::vec2 range = hf.range();
    // Clip the ray to the terrain's box.
    float t0 = 0.0f;
    float t1 = max_distance;
    const glm::vec3 lo(-half, range.x, -half);
    const glm::vec3 hi(half, range.y, half);
    for (int a = 0; a < 3; ++a) {
        if (std::abs(direction[a]) < 1e-12f) {
            if (origin[a] < lo[a] || origin[a] > hi[a]) {
                return std::nullopt;
            }
            continue;
        }
        float ta = (lo[a] - origin[a]) / direction[a];
        float tb = (hi[a] - origin[a]) / direction[a];
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) {
            return std::nullopt;
        }
    }
    // March at half a sample spacing; refine the crossing by bisection.
    const float step = hf.spacing() * 0.5f;
    const auto above = [&](float t) {
        const glm::vec3 p = origin + direction * t;
        return p.y - hf.height_at(p.x, p.z);
    };
    float prev_t = t0;
    float prev = above(t0);
    if (prev < 0.0f) {
        return origin + direction * t0; // starts below the surface (inside the box)
    }
    for (float t = t0 + step; t <= t1 + step; t += step) {
        const float tc = std::min(t, t1);
        const float cur = above(tc);
        if (cur <= 0.0f) {
            float a = prev_t;
            float b = tc;
            for (int i = 0; i < 24; ++i) {
                const float m = 0.5f * (a + b);
                (above(m) > 0.0f ? a : b) = m;
            }
            return origin + direction * b;
        }
        prev_t = tc;
        prev = cur;
        if (tc >= t1) {
            break;
        }
    }
    return std::nullopt;
}

glm::vec3 terrain_origin(scene::World& world, entt::entity terrain) {
    world.update_transforms();
    return glm::vec3(world.transform(terrain).world[3]);
}

std::optional<TerrainHit> raycast(scene::World& world, const glm::vec3& origin, const glm::vec3& direction) {
    std::optional<TerrainHit> best;
    for (const entt::entity e : world.registry().view<Terrain>()) {
        if (!scene::is_visible(world, e)) {
            continue;
        }
        const auto hf = surface_of(world, e);
        if (!hf) {
            continue;
        }
        const glm::vec3 base = terrain_origin(world, e);
        const auto hit = raycast(*hf, origin - base, direction);
        if (!hit) {
            continue;
        }
        const float distance = glm::length(*hit - (origin - base));
        if (!best || distance < best->distance) {
            best = TerrainHit{e, *hit + base, hf->normal_at(hit->x, hit->z), distance};
        }
    }
    return best;
}

} // namespace f3d::terrain
