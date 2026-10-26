// SPDX-License-Identifier: Apache-2.0
#include "f3d/world/spline.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/core/jobs.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/layers.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/world/scatter.hpp"

#include <entt/meta/factory.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace f3d::world {
namespace {

constexpr std::array<std::string_view, 5> kKindNames = {"road", "path", "river", "fence", "custom"};

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ bytes[i]) * 1099511628211ull;
    }
    return h;
}

constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;

/// A hash of everything about a spline entity that changes what it does.
std::uint64_t spline_hash(scene::World& world, entt::entity e, const Spline& spline) {
    nlohmann::json json;
    core::reflect::value_to_json(json, entt::forward_as_meta(spline));
    const std::string text = json.dump();
    std::uint64_t h = fnv(kFnvBasis, text.data(), text.size());
    const glm::vec3 at(world.transform(e).world[3]);
    h = fnv(h, &at, sizeof(at));
    const std::uint32_t id = static_cast<std::uint32_t>(entt::to_integral(e));
    return fnv(h, &id, sizeof(id));
}

float smoothstep(float e0, float e1, float x) noexcept {
    if (e1 <= e0) {
        return x < e0 ? 0.0f : 1.0f;
    }
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// A spline's effect on a terrain, in terrain-local metres.
struct Effect {
    SplineShape shape;
    float half = 0.0f;
    float falloff = 0.0f;
    float depth = 0.0f;
    bool shape_terrain = false;
    std::int32_t layer = -1;
    float strength = 1.0f;
};

/// The visible splines of a terrain that change it, in scene order.
std::vector<std::pair<entt::entity, const Spline*>> terrain_splines(scene::World& world, const core::Guid& terrain) {
    std::vector<std::pair<entt::entity, const Spline*>> out;
    for (const entt::entity e : world.depth_first()) {
        const Spline* s = world.registry().try_get<Spline>(e);
        if (!s || !s->visible || s->terrain != terrain || s->points.size() < 2 || !scene::is_visible(world, e)) {
            continue;
        }
        if (s->shape_terrain || s->paint_layer >= 0) {
            out.emplace_back(e, s);
        }
    }
    return out;
}

struct SurfaceState {
    std::weak_ptr<terrain::Heightfield> authored;
    std::shared_ptr<terrain::Heightfield> surface;
    std::uint64_t key = 0;
    std::uint32_t splat_maps = 0;
    std::vector<glm::uvec4> footprints; ///< tile rects (x0, z0, x1, z1) the effects covered
    std::vector<std::uint64_t> height_seen;
    std::vector<std::uint64_t> splat_seen;
};

struct ExclusionCache {
    std::uint64_t revision = 0;
    std::shared_ptr<const std::vector<std::pair<SplineShape, float>>> shapes; ///< shape, keep-clear distance
};

struct GeometryEntry {
    std::uint64_t key = 0;
    std::uint64_t surface_revision = 0;
    const terrain::Heightfield* surface = nullptr;
    SplineGeometry geometry;
};

/// Per-World state (registry context).
struct LandscapeState {
    std::unordered_map<core::Guid, SurfaceState> terrains;
    std::unordered_map<core::Guid, ExclusionCache> exclusions; ///< by scatter terrain
    std::unordered_map<entt::entity, GeometryEntry> geometry;
    std::uint64_t geometry_counter = 0;
};

LandscapeState& state_of(scene::World& world) {
    auto& ctx = world.registry().ctx();
    if (!ctx.contains<LandscapeState>()) {
        ctx.emplace<LandscapeState>();
    }
    return ctx.get<LandscapeState>();
}

/// Tiles (inclusive rect) a terrain-local x/z rectangle touches.
glm::uvec4 tile_rect(const terrain::Heightfield& hf, const glm::vec4& bounds) {
    const float half = hf.size() * 0.5f;
    const float tile = hf.spacing() * static_cast<float>(terrain::kTileQuads);
    const auto to_tile = [&](float v) {
        return static_cast<std::uint32_t>(
            std::clamp(std::floor((v + half) / tile), 0.0f, static_cast<float>(hf.tiles() - 1)));
    };
    return {to_tile(bounds.x), to_tile(bounds.y), to_tile(bounds.z), to_tile(bounds.w)};
}

/// Recomputes one tile of the surface: the authored data, then every effect. Writes
/// samples and weights only (safe in parallel for tiles that share no samples); the
/// caller marks the tile dirty and updates the bounds afterwards.
void rebuild_tile(const terrain::Heightfield& authored, terrain::Heightfield& surface, std::uint32_t tile,
                  const std::vector<Effect>& effects, std::uint32_t splat_maps) {
    const std::uint32_t tiles = authored.tiles();
    const std::uint32_t tx = tile % tiles;
    const std::uint32_t tz = tile / tiles;
    const std::uint32_t x0 = tx * terrain::kTileQuads;
    const std::uint32_t z0 = tz * terrain::kTileQuads;
    const std::uint32_t x1 = x0 + terrain::kTileQuads;
    const std::uint32_t z1 = z0 + terrain::kTileQuads;
    const std::uint32_t n = authored.samples();
    const float spacing = authored.spacing();
    const float half = authored.size() * 0.5f;
    for (std::uint32_t z = z0; z <= z1; ++z) {
        std::memcpy(surface.heights().data() + static_cast<std::size_t>(z) * n + x0,
                    authored.heights().data() + static_cast<std::size_t>(z) * n + x0, (x1 - x0 + 1) * sizeof(float));
    }
    for (const Effect& effect : effects) {
        if (!effect.shape_terrain) {
            continue;
        }
        const glm::vec4& b = effect.shape.bounds();
        for (std::uint32_t z = z0; z <= z1; ++z) {
            const float lz = static_cast<float>(z) * spacing - half;
            if (lz < b.y || lz > b.w) {
                continue;
            }
            for (std::uint32_t x = x0; x <= x1; ++x) {
                const float lx = static_cast<float>(x) * spacing - half;
                if (lx < b.x || lx > b.z) {
                    continue;
                }
                const auto hit = effect.shape.nearest(lx, lz);
                if (!hit) {
                    continue;
                }
                const float alpha = 1.0f - smoothstep(effect.half, effect.half + effect.falloff, hit->distance);
                if (alpha <= 0.0f) {
                    continue;
                }
                // Flat across the core, carved by `depth` in the middle (a rounded bed).
                const float r = effect.half > 0.0f ? std::min(hit->distance / effect.half, 1.0f) : 1.0f;
                const float target = hit->height - effect.depth * (1.0f - r * r);
                const float h = surface.at(x, z);
                surface.set(x, z, h + (target - h) * alpha);
            }
        }
    }
    // Layer weights: the authored ones, then the painting effects.
    const terrain::Splatmap* source = authored.splat();
    if (splat_maps == 0) {
        return;
    }
    terrain::Splatmap& splat = surface.ensure_splat(splat_maps * 4);
    const std::uint32_t res = splat.resolution();
    const std::uint32_t sx0 = tx * terrain::kSplatPerTile;
    const std::uint32_t sz0 = tz * terrain::kSplatPerTile;
    for (std::uint32_t m = 0; m < splat.maps(); ++m) {
        for (std::uint32_t z = sz0; z < sz0 + terrain::kSplatPerTile; ++z) {
            std::uint8_t* dst = splat.map(m) + (static_cast<std::size_t>(z) * res + sx0) * 4;
            if (source && m < source->maps()) {
                std::memcpy(dst, source->map(m) + (static_cast<std::size_t>(z) * res + sx0) * 4,
                            terrain::kSplatPerTile * 4);
            } else {
                std::memset(dst, 0, terrain::kSplatPerTile * 4);
                if (m == 0 && !source) {
                    for (std::uint32_t i = 0; i < terrain::kSplatPerTile; ++i) {
                        dst[i * 4] = 255; // layer 0 everywhere
                    }
                }
            }
        }
    }
    const float texel = authored.size() / static_cast<float>(res);
    for (const Effect& effect : effects) {
        if (effect.layer < 0 || static_cast<std::uint32_t>(effect.layer) >= splat.maps() * 4) {
            continue;
        }
        const glm::vec4& b = effect.shape.bounds();
        for (std::uint32_t z = sz0; z < sz0 + terrain::kSplatPerTile; ++z) {
            const float lz = (static_cast<float>(z) + 0.5f) * texel - half;
            if (lz < b.y || lz > b.w) {
                continue;
            }
            for (std::uint32_t x = sx0; x < sx0 + terrain::kSplatPerTile; ++x) {
                const float lx = (static_cast<float>(x) + 0.5f) * texel - half;
                if (lx < b.x || lx > b.z) {
                    continue;
                }
                const auto hit = effect.shape.nearest(lx, lz);
                if (!hit) {
                    continue;
                }
                const float a = (1.0f - smoothstep(effect.half * 0.8f, effect.half + effect.falloff * 0.5f, hit->distance)) *
                                effect.strength;
                terrain::paint_texel(splat, x, z, static_cast<std::uint32_t>(effect.layer), a);
            }
        }
    }
}

/// Appends a box (centred on x/z, from y0 to y1) to a mesh.
void add_box(assets::MeshData& mesh, const glm::vec3& lo, const glm::vec3& hi, std::uint32_t color) {
    const std::array<glm::vec3, 6> normals = {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                                              glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
    for (const glm::vec3& n : normals) {
        // Two axes spanning the face, chosen so the winding is counter-clockwise from outside.
        const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 v = glm::cross(n, u);
        const glm::vec3 centre = (lo + hi) * 0.5f;
        const glm::vec3 extent = (hi - lo) * 0.5f;
        const auto corner = [&](float a, float b) { return centre + (n + u * a + v * b) * extent; };
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const auto& [a, b] : {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f},
                                   std::pair{-1.0f, 1.0f}}) {
            const glm::vec3 p = corner(a, b);
            mesh.vertices.push_back({p, n, color});
            mesh.uvs.emplace_back((a + 1.0f) * 0.5f, (b + 1.0f) * 0.5f);
            mesh.bounds.add(p);
        }
        mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }
    mesh.primitive_count = static_cast<std::uint32_t>(mesh.indices.size() / 3);
}

/// Rotation taking +Z to `direction` (unit), keeping +Y up as far as possible.
glm::quat face(const glm::vec3& direction) noexcept {
    const float yaw = std::atan2(direction.x, direction.z);
    const float pitch = -std::asin(std::clamp(direction.y, -1.0f, 1.0f));
    return glm::angleAxis(yaw, glm::vec3(0, 1, 0)) * glm::angleAxis(pitch, glm::vec3(1, 0, 0));
}

} // namespace

// --- Names and presets ----------------------------------------------------------------------

std::string_view to_string(SplineKind kind) noexcept {
    return kKindNames[static_cast<std::size_t>(kind)];
}

bool parse(std::string_view text, SplineKind& kind) noexcept {
    for (std::size_t i = 0; i < kKindNames.size(); ++i) {
        if (text == kKindNames[i]) {
            kind = static_cast<SplineKind>(i);
            return true;
        }
    }
    return false;
}

Spline spline_preset(SplineKind kind) {
    Spline s;
    s.kind = kind;
    switch (kind) {
    case SplineKind::Road:
        s.width = 8.0f;
        s.falloff = 6.0f;
        s.surface = "asphalt";
        s.surface_tiling = 6.0f;
        s.clear_margin = 2.0f;
        break;
    case SplineKind::Path:
        s.width = 3.0f;
        s.falloff = 3.0f;
        s.surface.clear(); // painted (a dirt layer), not paved
        s.clear_margin = 0.5f;
        break;
    case SplineKind::River:
        s.width = 10.0f;
        s.falloff = 10.0f;
        s.depth = 3.0f;
        s.surface.clear();
        s.clear_margin = 2.0f;
        s.flow_speed = 1.2f;
        break;
    case SplineKind::Fence:
        s.width = 0.4f;
        s.shape_terrain = false;
        s.surface.clear();
        s.meshes = {{core::Guid{}, kBuiltinFencePost}, {core::Guid{}, kBuiltinFenceRail}};
        s.mesh_spacing = 2.5f;
        s.clear_margin = 0.6f;
        break;
    case SplineKind::Custom:
        s.width = 6.0f;
        s.surface.clear();
        break;
    }
    return s;
}

void register_spline_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        entt::meta_factory<SplineKind>{}
            .type("SplineKind")
            .data<SplineKind::Road>("Road")
            .data<SplineKind::Path>("Path")
            .data<SplineKind::River>("River")
            .data<SplineKind::Fence>("Fence")
            .data<SplineKind::Custom>("Custom");
        entt::meta_factory<Spline>{}
            .type("Spline")
            .data<&Spline::kind>("kind")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("What it is (settings come from it when created)"))
            .data<&Spline::terrain>("terrain")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("The terrain it shapes and paints"))
            .data<&Spline::points>("points")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip(
                "Control points relative to the entity (edit them with the Spline tool)"))
            .data<&Spline::closed>("closed")
            .custom<FieldMeta>(FieldMeta{}.with_label("Loop").with_tooltip("Join the last point back to the first"))
            .data<&Spline::width>("width")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 500.0, 0.1))
            .data<&Spline::shape_terrain>("shape_terrain")
            .custom<FieldMeta>(FieldMeta{}.with_label("Shape terrain").with_tooltip(
                "Flatten the ground to the curve (and carve the depth)"))
            .data<&Spline::falloff>("falloff")
            .custom<FieldMeta>(FieldMeta{}.with_label("Blend").with_unit("m").with_range(0.0, 200.0, 0.1).with_tooltip(
                "How far beyond each edge the ground eases back to its own shape"))
            .data<&Spline::depth>("depth")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(-50.0, 50.0, 0.05).with_tooltip(
                "Carved below the curve at the centre: a riverbed or a ditch (negative: a ridge)"))
            .data<&Spline::paint_layer>("paint_layer")
            .custom<FieldMeta>(FieldMeta{}.with_label("Paint layer").with_range(-1.0, 15.0, 1.0).with_tooltip(
                "Terrain layer painted under it (-1: none)"))
            .data<&Spline::paint_strength>("paint_strength")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01))
            .data<&Spline::surface>("surface")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip(
                "Built-in surface laid along it: asphalt, gravel, cobblestone, dirt... (empty: none)"))
            .data<&Spline::surface_tiling>("surface_tiling")
            .custom<FieldMeta>(FieldMeta{}.with_label("Surface tiling").with_unit("m").with_range(0.25, 100.0, 0.05))
            .data<&Spline::meshes>("meshes")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Meshes placed along it in turn (posts, lamps, trees)"))
            .data<&Spline::mesh_spacing>("mesh_spacing")
            .custom<FieldMeta>(FieldMeta{}.with_label("Mesh spacing").with_unit("m").with_range(0.25, 1000.0, 0.05))
            .data<&Spline::mesh_offset>("mesh_offset")
            .custom<FieldMeta>(FieldMeta{}.with_label("Mesh offset").with_unit("m").with_range(-100.0, 100.0, 0.05)
                                   .with_tooltip("Sideways distance of the meshes from the curve"))
            .data<&Spline::both_sides>("both_sides")
            .custom<FieldMeta>(FieldMeta{}.with_label("Both sides"))
            .data<&Spline::clear_scatter>("clear_scatter")
            .custom<FieldMeta>(FieldMeta{}.with_label("Clear scatter").with_tooltip(
                "Keep trees, grass and rocks off it"))
            .data<&Spline::clear_margin>("clear_margin")
            .custom<FieldMeta>(FieldMeta{}.with_label("Clear margin").with_unit("m").with_range(0.0, 100.0, 0.1))
            .data<&Spline::flow_speed>("flow_speed")
            .custom<FieldMeta>(FieldMeta{}.with_label("Flow speed").with_unit("m/s").with_range(0.0, 20.0, 0.05)
                                   .with_tooltip("Rivers: how fast the water flows along the curve"))
            .data<&Spline::visible>("visible");
        scene::register_component<Spline>("Spline", true);
        return true;
    }();
    static_cast<void>(registered);
}

// --- Curves ---------------------------------------------------------------------------------

std::vector<SplineSample> evaluate_spline(const std::vector<glm::vec3>& points, bool closed, float step) {
    std::vector<SplineSample> out;
    const std::size_t n = points.size();
    if (n == 0) {
        return out;
    }
    if (n == 1) {
        out.push_back({points[0], glm::vec3(0, 0, 1), 0.0f});
        return out;
    }
    step = std::max(step, 0.01f);
    const bool loop = closed && n >= 3;
    const std::size_t segments = loop ? n : n - 1;
    const auto point = [&](std::ptrdiff_t i) -> glm::vec3 {
        const auto count = static_cast<std::ptrdiff_t>(n);
        if (loop) {
            return points[static_cast<std::size_t>(((i % count) + count) % count)];
        }
        if (i < 0) {
            return points[0] * 2.0f - points[1]; // extrapolated ends
        }
        if (i >= count) {
            return points[n - 1] * 2.0f - points[n - 2];
        }
        return points[static_cast<std::size_t>(i)];
    };
    std::vector<glm::vec3> positions;
    for (std::size_t s = 0; s < segments; ++s) {
        const auto i = static_cast<std::ptrdiff_t>(s);
        const glm::vec3 p0 = point(i - 1), p1 = point(i), p2 = point(i + 1), p3 = point(i + 2);
        // Centripetal parametrisation (alpha 0.5): no cusps or self-intersections.
        const auto knot = [](const glm::vec3& a, const glm::vec3& b) {
            return std::max(std::sqrt(glm::length(b - a)), 1e-4f);
        };
        const float t0 = 0.0f;
        const float t1 = t0 + knot(p0, p1);
        const float t2 = t1 + knot(p1, p2);
        const float t3 = t2 + knot(p2, p3);
        const auto steps = static_cast<std::size_t>(std::max(1.0f, std::ceil(glm::length(p2 - p1) / step)));
        for (std::size_t k = 0; k < steps; ++k) {
            const float t = t1 + (t2 - t1) * static_cast<float>(k) / static_cast<float>(steps);
            const glm::vec3 a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1;
            const glm::vec3 a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2;
            const glm::vec3 a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3;
            const glm::vec3 b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
            const glm::vec3 b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
            positions.push_back((t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2);
        }
    }
    positions.push_back(loop ? points[0] : points[n - 1]);
    out.resize(positions.size());
    float distance = 0.0f;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (i > 0) {
            distance += glm::length(positions[i] - positions[i - 1]);
        }
        const glm::vec3 ahead = positions[std::min(i + 1, positions.size() - 1)];
        const glm::vec3 behind = positions[i > 0 ? i - 1 : 0];
        const glm::vec3 d = ahead - behind;
        out[i] = {positions[i], glm::length(d) > 1e-6f ? glm::normalize(d) : glm::vec3(0, 0, 1), distance};
    }
    return out;
}

std::vector<glm::vec3> spline_world_points(scene::World& world, entt::entity e) {
    std::vector<glm::vec3> out;
    const Spline* spline = world.valid(e) ? world.registry().try_get<Spline>(e) : nullptr;
    if (!spline) {
        return out;
    }
    world.update_transforms();
    const glm::vec3 origin(world.transform(e).world[3]);
    out.reserve(spline->points.size());
    for (const glm::vec3& p : spline->points) {
        out.push_back(origin + p);
    }
    return out;
}

SplineShape::SplineShape(const std::vector<SplineSample>& samples, float reach) : reach_(std::max(reach, 0.01f)) {
    if (samples.size() < 2) {
        return;
    }
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
        segments_.push_back({samples[i].position, samples[i + 1].position, samples[i].distance, samples[i + 1].distance});
        lo = glm::min(lo, glm::min(glm::vec2(samples[i].position.x, samples[i].position.z),
                                   glm::vec2(samples[i + 1].position.x, samples[i + 1].position.z)));
        hi = glm::max(hi, glm::max(glm::vec2(samples[i].position.x, samples[i].position.z),
                                   glm::vec2(samples[i + 1].position.x, samples[i + 1].position.z)));
    }
    bounds_ = glm::vec4(lo - reach_, hi + reach_);
    cell_ = std::max(reach_, 4.0f);
    origin_ = glm::ivec2(glm::floor(glm::vec2(bounds_.x, bounds_.y) / cell_));
    const glm::ivec2 last(glm::floor(glm::vec2(bounds_.z, bounds_.w) / cell_));
    size_ = last - origin_ + 1;
    cells_.resize(static_cast<std::size_t>(size_.x) * static_cast<std::size_t>(size_.y));
    for (std::uint32_t s = 0; s < segments_.size(); ++s) {
        const Segment& seg = segments_[s];
        const glm::vec2 a(std::min(seg.a.x, seg.b.x) - reach_, std::min(seg.a.z, seg.b.z) - reach_);
        const glm::vec2 b(std::max(seg.a.x, seg.b.x) + reach_, std::max(seg.a.z, seg.b.z) + reach_);
        const glm::ivec2 c0 = glm::ivec2(glm::floor(a / cell_)) - origin_;
        const glm::ivec2 c1 = glm::ivec2(glm::floor(b / cell_)) - origin_;
        for (int z = std::max(c0.y, 0); z <= std::min(c1.y, size_.y - 1); ++z) {
            for (int x = std::max(c0.x, 0); x <= std::min(c1.x, size_.x - 1); ++x) {
                cells_[static_cast<std::size_t>(z) * static_cast<std::size_t>(size_.x) + static_cast<std::size_t>(x)]
                    .push_back(s);
            }
        }
    }
}

std::optional<SplineShape::Hit> SplineShape::nearest(float x, float z) const {
    if (segments_.empty() || x < bounds_.x || x > bounds_.z || z < bounds_.y || z > bounds_.w) {
        return std::nullopt;
    }
    const glm::ivec2 c = glm::ivec2(glm::floor(glm::vec2(x, z) / cell_)) - origin_;
    if (c.x < 0 || c.y < 0 || c.x >= size_.x || c.y >= size_.y) {
        return std::nullopt;
    }
    std::optional<Hit> best;
    const glm::vec2 p(x, z);
    for (const std::uint32_t s :
         cells_[static_cast<std::size_t>(c.y) * static_cast<std::size_t>(size_.x) + static_cast<std::size_t>(c.x)]) {
        const Segment& seg = segments_[s];
        const glm::vec2 a(seg.a.x, seg.a.z);
        const glm::vec2 d = glm::vec2(seg.b.x, seg.b.z) - a;
        const float len2 = glm::dot(d, d);
        const float t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f) : 0.0f;
        const float distance = glm::length(p - (a + d * t));
        if (distance <= reach_ && (!best || distance < best->distance)) {
            best = Hit{distance, seg.a.y + (seg.b.y - seg.a.y) * t, seg.along_a + (seg.along_b - seg.along_a) * t};
        }
    }
    return best;
}

// --- Landscape ------------------------------------------------------------------------------

std::uint64_t splines_revision(scene::World& world) {
    world.update_transforms();
    std::uint64_t h = kFnvBasis;
    for (const entt::entity e : world.registry().view<Spline>()) {
        const std::uint64_t one = spline_hash(world, e, world.registry().get<Spline>(e));
        h = fnv(h, &one, sizeof(one));
    }
    return h;
}

void update_landscape(scene::World& world) {
    F3D_ZONE;
    world.update_transforms();
    LandscapeState& state = state_of(world);
    terrain::HeightfieldStore& store = terrain::heightfields(world);
    std::vector<core::Guid> live;
    for (const entt::entity t : world.registry().view<terrain::Terrain>()) {
        const core::Guid guid = world.guid(t);
        live.push_back(guid);
        const std::shared_ptr<terrain::Heightfield> authored = terrain::heightfield_of(world, t);
        const auto splines = authored ? terrain_splines(world, guid) : decltype(terrain_splines(world, guid)){};
        if (splines.empty()) {
            if (state.terrains.erase(guid) > 0) {
                store.erase_surface(guid); // back to the authored terrain
            }
            continue;
        }
        const glm::vec3 origin = terrain::terrain_origin(world, t);
        std::uint64_t key = fnv(kFnvBasis, &origin, sizeof(origin));
        std::vector<Effect> effects;
        std::uint32_t needed_maps = authored->splat() ? authored->splat()->maps() : 0;
        for (const auto& [e, spline] : splines) {
            const std::uint64_t one = spline_hash(world, e, *spline);
            key = fnv(key, &one, sizeof(one));
            std::vector<glm::vec3> points = spline_world_points(world, e);
            for (glm::vec3& p : points) {
                p -= origin;
            }
            const float half = std::max(spline->width, 0.0f) * 0.5f;
            const float falloff = std::max(spline->falloff, 0.0f);
            Effect effect{SplineShape(evaluate_spline(points, spline->closed, std::clamp(half * 0.5f, 0.25f, 2.0f)),
                                      half + falloff),
                          half,
                          falloff,
                          spline->depth,
                          spline->shape_terrain,
                          spline->paint_layer < static_cast<std::int32_t>(terrain::kMaxLayers) ? spline->paint_layer : -1,
                          std::clamp(spline->paint_strength, 0.0f, 1.0f)};
            if (effect.layer >= 0) {
                needed_maps = std::max(needed_maps, static_cast<std::uint32_t>(effect.layer) / 4 + 1);
            }
            if (!effect.shape.empty()) {
                effects.push_back(std::move(effect));
            }
        }

        SurfaceState& s = state.terrains[guid];
        const std::uint32_t tiles = authored->tiles() * authored->tiles();
        const bool rebuild_all = !s.surface || s.authored.lock() != authored || s.splat_maps != needed_maps ||
                                 s.height_seen.size() != tiles;
        std::vector<bool> dirty(tiles, rebuild_all);
        std::vector<glm::uvec4> footprints;
        for (const Effect& effect : effects) {
            footprints.push_back(tile_rect(*authored, effect.shape.bounds()));
        }
        const auto mark = [&](const glm::uvec4& r) {
            for (std::uint32_t z = r.y; z <= r.w; ++z) {
                for (std::uint32_t x = r.x; x <= r.z; ++x) {
                    dirty[z * authored->tiles() + x] = true;
                }
            }
        };
        if (!rebuild_all) {
            if (key != s.key) {
                for (const glm::uvec4& r : s.footprints) {
                    mark(r);
                }
                for (const glm::uvec4& r : footprints) {
                    mark(r);
                }
            }
            for (std::uint32_t i = 0; i < tiles; ++i) {
                const bool heights_changed = authored->tile_revision(i) != s.height_seen[i];
                const bool splat_changed = authored->splat() && authored->splat()->tile_revision(i) != s.splat_seen[i];
                dirty[i] = dirty[i] || heights_changed || splat_changed;
            }
        } else {
            s.surface = std::make_shared<terrain::Heightfield>(authored->tiles(), authored->size());
            s.authored = authored;
            s.splat_maps = needed_maps;
            s.height_seen.assign(tiles, 0);
            s.splat_seen.assign(tiles, 0);
            store.set_surface(guid, s.surface);
        }
        std::vector<std::uint32_t> work;
        for (std::uint32_t i = 0; i < tiles; ++i) {
            if (dirty[i]) {
                work.push_back(i);
            }
        }
        if (!work.empty()) {
            F3D_ZONE_N("Rebuild terrain surface tiles");
            if (needed_maps > 0) {
                s.surface->ensure_splat(needed_maps * 4); // before the parallel loop
            }
            // Tiles share their edge samples: four passes, so tiles running together
            // are at least one tile apart.
            for (std::uint32_t parity = 0; parity < 4; ++parity) {
                std::vector<std::uint32_t> pass;
                for (const std::uint32_t tile : work) {
                    const std::uint32_t tx = tile % authored->tiles();
                    const std::uint32_t tz = tile / authored->tiles();
                    if ((tx & 1u) + 2u * (tz & 1u) == parity) {
                        pass.push_back(tile);
                    }
                }
                core::jobs::parallel_for(
                    pass.size(),
                    [&](std::size_t begin, std::size_t end) {
                        for (std::size_t i = begin; i < end; ++i) {
                            rebuild_tile(*authored, *s.surface, pass[i], effects, needed_maps);
                        }
                    },
                    1);
            }
            for (const std::uint32_t tile : work) {
                const std::uint32_t x0 = (tile % authored->tiles()) * terrain::kTileQuads;
                const std::uint32_t z0 = (tile / authored->tiles()) * terrain::kTileQuads;
                s.surface->mark_dirty(x0, z0, x0 + terrain::kTileQuads, z0 + terrain::kTileQuads);
                s.surface->update_bounds(x0, z0, x0 + terrain::kTileQuads, z0 + terrain::kTileQuads);
                if (s.surface->splat()) {
                    s.surface->splat()->mark_dirty(tile);
                }
            }
        }
        for (std::uint32_t i = 0; i < tiles; ++i) {
            s.height_seen[i] = authored->tile_revision(i);
            s.splat_seen[i] = authored->splat() ? authored->splat()->tile_revision(i) : 0;
        }
        s.key = key;
        s.footprints = std::move(footprints);
    }
    // Terrains that are gone.
    for (auto it = state.terrains.begin(); it != state.terrains.end();) {
        if (std::find(live.begin(), live.end(), it->first) == live.end()) {
            store.erase_surface(it->first);
            it = state.terrains.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(state.geometry, [&](const auto& entry) {
        return !world.valid(entry.first) || !world.registry().all_of<Spline>(entry.first);
    });
}

std::function<bool(float x, float z)> scatter_exclusion(scene::World& world, const Scatter& scatter,
                                                        std::uint64_t& revision) {
    revision = splines_revision(world);
    ExclusionCache& cache = state_of(world).exclusions[scatter.terrain];
    if (!cache.shapes || cache.revision != revision) {
        auto shapes = std::make_shared<std::vector<std::pair<SplineShape, float>>>();
        for (const entt::entity e : world.registry().view<Spline>()) {
            const Spline& spline = world.registry().get<Spline>(e);
            if (!spline.visible || !spline.clear_scatter || spline.points.size() < 2 ||
                (!scatter.terrain.is_null() && spline.terrain != scatter.terrain)) {
                continue;
            }
            const float keep = std::max(spline.width, 0.0f) * 0.5f + std::max(spline.clear_margin, 0.0f);
            shapes->emplace_back(SplineShape(evaluate_spline(spline_world_points(world, e), spline.closed, 1.0f), keep),
                                 keep);
        }
        cache.shapes = std::move(shapes);
        cache.revision = revision;
    }
    if (cache.shapes->empty()) {
        return {};
    }
    return [shapes = cache.shapes](float x, float z) {
        for (const auto& [shape, keep] : *shapes) {
            if (const auto hit = shape.nearest(x, z); hit && hit->distance <= keep) {
                return true;
            }
        }
        return false;
    };
}

// --- Geometry -------------------------------------------------------------------------------

const SplineGeometry& spline_geometry(scene::World& world, entt::entity e) {
    static const SplineGeometry kNone;
    const Spline* spline = world.valid(e) ? world.registry().try_get<Spline>(e) : nullptr;
    if (!spline || spline->points.size() < 2) {
        return kNone;
    }
    LandscapeState& state = state_of(world);
    GeometryEntry& entry = state.geometry[e];
    // The ground it lies on.
    const entt::entity t = spline->terrain.is_null() ? entt::null : world.find(spline->terrain);
    const std::shared_ptr<terrain::Heightfield> ground = terrain::surface_of(world, t);
    const glm::vec3 origin = ground ? terrain::terrain_origin(world, t) : glm::vec3(0.0f);
    const std::uint64_t key = spline_hash(world, e, *spline);
    const std::uint64_t ground_revision = ground ? ground->revision() : 0;
    if (entry.key == key && entry.surface == ground.get() && entry.surface_revision == ground_revision &&
        entry.geometry.revision != 0) {
        return entry.geometry;
    }
    F3D_ZONE_N("Build spline geometry");
    entry.key = key;
    entry.surface = ground.get();
    entry.surface_revision = ground_revision;
    SplineGeometry& g = entry.geometry;
    g = {};
    g.revision = ++state.geometry_counter;

    const auto height_at = [&](const glm::vec3& p) {
        if (!ground) {
            return p.y;
        }
        return ground->height_at(p.x - origin.x, p.z - origin.z) + origin.y;
    };
    const auto normal_at = [&](const glm::vec3& p) {
        return ground ? ground->normal_at(p.x - origin.x, p.z - origin.z) : glm::vec3(0, 1, 0);
    };
    const std::vector<glm::vec3> points = spline_world_points(world, e);
    const float width = std::max(spline->width, 0.01f);

    // The surface ribbon, draped over the (shaped) ground.
    if (!spline->surface.empty()) {
        const std::vector<SplineSample> samples = evaluate_spline(points, spline->closed, std::clamp(width / 4.0f, 0.5f, 2.0f));
        auto mesh = std::make_shared<assets::MeshData>();
        mesh->name = "Spline surface";
        constexpr std::uint32_t kColumns = 5;
        const float tiling = std::max(spline->surface_tiling, 0.01f);
        for (const SplineSample& s : samples) {
            glm::vec3 right = glm::cross(s.tangent, glm::vec3(0, 1, 0)); // points left of the direction
            right = glm::length(right) > 1e-5f ? glm::normalize(right) : glm::vec3(1, 0, 0);
            for (std::uint32_t c = 0; c < kColumns; ++c) {
                const float u = static_cast<float>(c) / static_cast<float>(kColumns - 1);
                glm::vec3 p = s.position + right * ((u - 0.5f) * width);
                p.y = (ground ? height_at(p) : s.position.y) + 0.06f;
                mesh->vertices.push_back({p, normal_at(p), 0xFFFFFFFFu});
                mesh->uvs.emplace_back((u - 0.5f) * width / tiling, s.distance / tiling);
                mesh->bounds.add(p);
            }
        }
        for (std::uint32_t r = 0; r + 1 < samples.size(); ++r) {
            for (std::uint32_t c = 0; c + 1 < kColumns; ++c) {
                const std::uint32_t a = r * kColumns + c;
                const std::uint32_t b = a + kColumns;
                // Counter-clockwise seen from above (columns run right to left).
                mesh->indices.insert(mesh->indices.end(), {a, a + 1, b, a + 1, b + 1, b});
            }
        }
        mesh->primitive_count = static_cast<std::uint32_t>(mesh->indices.size() / 3);
        mesh->submeshes = {{0, static_cast<std::uint32_t>(mesh->indices.size()), 0}};
        g.surface = std::move(mesh);
    }

    // Meshes along it; fence rails stretch between consecutive placements.
    std::vector<core::AssetRef> singles;
    bool rails = false;
    for (const core::AssetRef& m : spline->meshes) {
        if (m.path == kBuiltinFenceRail) {
            rails = true;
        } else if (!m.empty()) {
            singles.push_back(m);
        }
    }
    if (!singles.empty() || rails) {
        const std::vector<SplineSample> samples = evaluate_spline(points, spline->closed, 0.5f);
        const float total = samples.back().distance;
        const float spacing = std::max(spline->mesh_spacing, 0.1f);
        const std::vector<float> sides = spline->both_sides && spline->mesh_offset != 0.0f
                                             ? std::vector<float>{spline->mesh_offset, -spline->mesh_offset}
                                             : std::vector<float>{spline->mesh_offset};
        for (const float side : sides) {
            std::vector<glm::vec3> placed;
            std::size_t cursor = 0;
            std::size_t index = 0;
            for (float d = 0.0f; d <= total + 1e-3f; d += spacing, ++index) {
                while (cursor + 1 < samples.size() && samples[cursor + 1].distance < d) {
                    ++cursor;
                }
                const SplineSample& a = samples[cursor];
                const SplineSample& b = samples[std::min(cursor + 1, samples.size() - 1)];
                const float span = b.distance - a.distance;
                const float f = span > 1e-6f ? std::clamp((d - a.distance) / span, 0.0f, 1.0f) : 0.0f;
                const glm::vec3 tangent = glm::normalize(glm::mix(a.tangent, b.tangent, f) + glm::vec3(0, 0, 1e-6f));
                glm::vec3 right = glm::cross(tangent, glm::vec3(0, 1, 0));
                right = glm::length(right) > 1e-5f ? glm::normalize(right) : glm::vec3(1, 0, 0);
                glm::vec3 p = glm::mix(a.position, b.position, f) + right * side;
                p.y = height_at(p);
                placed.push_back(p);
                if (!singles.empty()) {
                    const glm::vec3 flat = glm::normalize(glm::vec3(tangent.x, 0.0f, tangent.z) + glm::vec3(0, 0, 1e-6f));
                    g.props.push_back({singles[index % singles.size()],
                                       glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(face(flat))});
                }
            }
            if (rails) {
                for (std::size_t i = 0; i + 1 < placed.size(); ++i) {
                    const glm::vec3 d = placed[i + 1] - placed[i];
                    const float length = glm::length(d);
                    if (length < 1e-3f) {
                        continue;
                    }
                    for (const float h : {0.45f, 0.95f}) {
                        const glm::vec3 mid = (placed[i] + placed[i + 1]) * 0.5f + glm::vec3(0.0f, h, 0.0f);
                        g.props.push_back({{core::Guid{}, kBuiltinFenceRail},
                                           glm::translate(glm::mat4(1.0f), mid) * glm::mat4_cast(face(d / length)) *
                                               glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, length))});
                    }
                }
            }
        }
    }

    // A river's water: a strip at the curve's height, a little wider than the bed.
    if (spline->kind == SplineKind::River) {
        const std::vector<SplineSample> samples = evaluate_spline(points, spline->closed, std::clamp(width / 4.0f, 0.5f, 3.0f));
        auto strip = std::make_shared<WaterStrip>();
        constexpr std::uint32_t kColumns = 4;
        const float water_width = width + std::max(spline->falloff, 0.0f) * 0.6f;
        for (const SplineSample& s : samples) {
            glm::vec3 right = glm::cross(s.tangent, glm::vec3(0, 1, 0));
            right = glm::length(right) > 1e-5f ? glm::normalize(right) : glm::vec3(1, 0, 0);
            const glm::vec2 flow = glm::vec2(s.tangent.x, s.tangent.z) * spline->flow_speed;
            for (std::uint32_t c = 0; c < kColumns; ++c) {
                const float u = static_cast<float>(c) / static_cast<float>(kColumns - 1);
                strip->positions.push_back(s.position + right * ((u - 0.5f) * water_width));
                strip->flow.push_back(flow);
            }
        }
        for (std::uint32_t r = 0; r + 1 < samples.size(); ++r) {
            for (std::uint32_t c = 0; c + 1 < kColumns; ++c) {
                const std::uint32_t a = r * kColumns + c;
                const std::uint32_t b = a + kColumns;
                strip->indices.insert(strip->indices.end(), {a, a + 1, b, a + 1, b + 1, b});
            }
        }
        g.water = std::move(strip);
    }
    return g;
}

std::vector<std::pair<std::string, assets::MeshData>> builtin_spline_meshes() {
    const std::uint32_t wood = core::debug::rgba8(112, 82, 54);
    assets::MeshData post;
    post.name = "Fence post";
    add_box(post, {-0.06f, 0.0f, -0.06f}, {0.06f, 1.2f, 0.06f}, wood);
    assets::MeshData rail;
    rail.name = "Fence rail";
    add_box(rail, {-0.03f, -0.05f, -0.5f}, {0.03f, 0.05f, 0.5f}, wood);
    return {{kBuiltinFencePost, std::move(post)}, {kBuiltinFenceRail, std::move(rail)}};
}

} // namespace f3d::world
