// SPDX-License-Identifier: Apache-2.0
#include "f3d/world/water.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace f3d::world {
namespace {

constexpr std::array<std::string_view, 2> kKindNames = {"lake", "ocean"};

struct LakeEntry {
    WaterBody key;
    glm::vec3 position{0.0f};
    const terrain::Heightfield* heights = nullptr;
    std::uint64_t revision = 0;
    glm::vec3 terrain_origin{0.0f};
    std::shared_ptr<const WaterStrip> surface;
    bool valid = false;
};

struct LakeCache {
    std::unordered_map<entt::entity, LakeEntry> entries;
};

/// The terrain a lake fills: its own, else the first whose square holds the entity.
entt::entity lake_terrain(scene::World& world, const WaterBody& water, const glm::vec3& at) {
    if (!water.terrain.is_null()) {
        const entt::entity t = world.find(water.terrain);
        return t != entt::null && world.registry().all_of<terrain::Terrain>(t) ? t : entt::null;
    }
    for (const entt::entity t : world.registry().view<terrain::Terrain>()) {
        const glm::vec3 o = terrain::terrain_origin(world, t);
        const float half = world.registry().get<terrain::Terrain>(t).size * 0.5f;
        if (std::abs(at.x - o.x) <= half && std::abs(at.z - o.z) <= half) {
            return t;
        }
    }
    return entt::null;
}

} // namespace

std::string_view to_string(WaterKind kind) noexcept {
    return kKindNames[static_cast<std::size_t>(kind)];
}

bool parse(std::string_view text, WaterKind& kind) noexcept {
    for (std::size_t i = 0; i < kKindNames.size(); ++i) {
        if (text == kKindNames[i]) {
            kind = static_cast<WaterKind>(i);
            return true;
        }
    }
    return false;
}

WaterBody water_preset(WaterKind kind) {
    WaterBody w;
    w.kind = kind;
    if (kind == WaterKind::Ocean) {
        w.shallow_color = {0.06f, 0.32f, 0.36f};
        w.deep_color = {0.004f, 0.022f, 0.050f};
        w.clarity = 8.0f;
        w.wave_height = 0.9f;
        w.wave_length = 28.0f;
        w.wave_speed = 1.0f;
        w.foam = 0.8f;
        w.roughness = 0.06f;
    }
    return w;
}

void register_water_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        entt::meta_factory<WaterKind>{}.type("WaterKind").data<WaterKind::Lake>("Lake").data<WaterKind::Ocean>("Ocean");
        entt::meta_factory<WaterBody>{}
            .type("WaterBody")
            .data<&WaterBody::kind>("kind")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip(
                "Lake: fills the basin under the entity up to its height. Ocean: an endless plane at its height"))
            .data<&WaterBody::terrain>("terrain")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Lakes: the terrain whose basin fills (none: the one below)"))
            .data<&WaterBody::extent>("extent")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_tooltip(
                "Lakes: the largest area the water may spread over (x, z), centred on the entity"))
            .data<&WaterBody::shallow_color>("shallow_color")
            .custom<FieldMeta>(FieldMeta{}.with_label("Shallow colour").with_color())
            .data<&WaterBody::deep_color>("deep_color")
            .custom<FieldMeta>(FieldMeta{}.with_label("Deep colour").with_color())
            .data<&WaterBody::clarity>("clarity")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 100.0, 0.1).with_tooltip(
                "How deep you can see before the bottom fades into the deep colour"))
            .data<&WaterBody::wave_height>("wave_height")
            .custom<FieldMeta>(FieldMeta{}.with_label("Wave height").with_unit("m").with_range(0.0, 10.0, 0.01))
            .data<&WaterBody::wave_length>("wave_length")
            .custom<FieldMeta>(FieldMeta{}.with_label("Wave length").with_unit("m").with_range(0.5, 300.0, 0.1))
            .data<&WaterBody::wind_direction>("wind_direction")
            .custom<FieldMeta>(FieldMeta{}.with_label("Wind direction").with_unit("deg").with_range(-180.0, 180.0, 1.0))
            .data<&WaterBody::wave_speed>("wave_speed")
            .custom<FieldMeta>(FieldMeta{}.with_label("Wave speed").with_range(0.0, 5.0, 0.01))
            .data<&WaterBody::foam>("foam")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 2.0, 0.01).with_tooltip("Foam along shores and on crests"))
            .data<&WaterBody::refraction>("refraction")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 2.0, 0.01).with_tooltip(
                "How much the bottom wobbles seen through the waves"))
            .data<&WaterBody::roughness>("roughness")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.005).with_tooltip("0: mirror-like reflections"))
            .data<&WaterBody::visible>("visible");
        scene::register_component<WaterBody>("WaterBody", true);
        return true;
    }();
    static_cast<void>(registered);
}

FloodResult flood_basin(const terrain::Heightfield& heights, const glm::vec2& seed, float level,
                        const glm::vec2& extent, std::uint32_t max_cells) {
    F3D_ZONE;
    FloodResult out;
    const float half_size = heights.size() * 0.5f;
    // The searched rectangle, clipped to the terrain.
    const glm::vec2 lo = glm::max(seed - glm::max(extent, glm::vec2(1.0f)) * 0.5f, glm::vec2(-half_size));
    const glm::vec2 hi = glm::min(seed + glm::max(extent, glm::vec2(1.0f)) * 0.5f, glm::vec2(half_size));
    if (hi.x <= lo.x || hi.y <= lo.y) {
        return out;
    }
    const float longest = std::max(hi.x - lo.x, hi.y - lo.y);
    out.step = std::max(heights.spacing(), longest / static_cast<float>(std::max(max_cells, 2u)));
    out.width = static_cast<std::uint32_t>(std::ceil((hi.x - lo.x) / out.step));
    out.height = static_cast<std::uint32_t>(std::ceil((hi.y - lo.y) / out.step));
    out.origin = lo;
    out.wet.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    const auto below = [&](std::uint32_t x, std::uint32_t z) {
        const float lx = out.origin.x + (static_cast<float>(x) + 0.5f) * out.step;
        const float lz = out.origin.y + (static_cast<float>(z) + 0.5f) * out.step;
        return heights.height_at(lx, lz) < level;
    };
    const auto sx = static_cast<std::int64_t>(std::floor((seed.x - out.origin.x) / out.step));
    const auto sz = static_cast<std::int64_t>(std::floor((seed.y - out.origin.y) / out.step));
    if (sx < 0 || sz < 0 || sx >= out.width || sz >= out.height ||
        !below(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sz))) {
        return out;
    }
    std::vector<std::uint32_t> stack = {static_cast<std::uint32_t>(sz) * out.width + static_cast<std::uint32_t>(sx)};
    out.wet[stack.back()] = 1;
    while (!stack.empty()) {
        const std::uint32_t cell = stack.back();
        stack.pop_back();
        ++out.cells;
        const std::uint32_t x = cell % out.width;
        const std::uint32_t z = cell / out.width;
        const auto visit = [&](std::uint32_t nx, std::uint32_t nz) {
            const std::uint32_t n = nz * out.width + nx;
            if (!out.wet[n] && below(nx, nz)) {
                out.wet[n] = 1;
                stack.push_back(n);
            }
        };
        if (x > 0) {
            visit(x - 1, z);
        }
        if (x + 1 < out.width) {
            visit(x + 1, z);
        }
        if (z > 0) {
            visit(x, z - 1);
        }
        if (z + 1 < out.height) {
            visit(x, z + 1);
        }
    }
    return out;
}

float water_level(scene::World& world, entt::entity water) {
    world.update_transforms();
    return world.transform(water).world[3].y;
}

std::shared_ptr<const WaterStrip> lake_surface(scene::World& world, entt::entity e) {
    const WaterBody* water = world.valid(e) ? world.registry().try_get<WaterBody>(e) : nullptr;
    if (!water || water->kind != WaterKind::Lake) {
        return nullptr;
    }
    world.update_transforms();
    const glm::vec3 at(world.transform(e).world[3]);
    const entt::entity t = lake_terrain(world, *water, at);
    const std::shared_ptr<terrain::Heightfield> heights = terrain::surface_of(world, t);
    if (!heights) {
        return nullptr;
    }
    const glm::vec3 origin = terrain::terrain_origin(world, t);
    auto& ctx = world.registry().ctx();
    if (!ctx.contains<LakeCache>()) {
        ctx.emplace<LakeCache>();
    }
    LakeEntry& entry = ctx.get<LakeCache>().entries[e];
    if (entry.valid && entry.key == *water && entry.position == at && entry.heights == heights.get() &&
        entry.revision == heights->revision() && entry.terrain_origin == origin) {
        return entry.surface;
    }
    F3D_ZONE_N("Flood lake");
    entry.key = *water;
    entry.position = at;
    entry.heights = heights.get();
    entry.revision = heights->revision();
    entry.terrain_origin = origin;
    entry.valid = true;
    entry.surface.reset();

    const FloodResult flood =
        flood_basin(*heights, glm::vec2(at.x - origin.x, at.z - origin.z), at.y - origin.y, water->extent);
    if (flood.cells == 0) {
        return nullptr;
    }
    // Cells to cover: the wet ones and a one-cell skirt that tucks under the shore.
    const std::uint32_t w = flood.width;
    const std::uint32_t h = flood.height;
    std::vector<std::uint8_t> cover(flood.wet.size(), 0);
    for (std::uint32_t z = 0; z < h; ++z) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (!flood.wet[static_cast<std::size_t>(z) * w + x]) {
                continue;
            }
            for (std::uint32_t nz = z > 0 ? z - 1 : 0; nz <= std::min(z + 1, h - 1); ++nz) {
                for (std::uint32_t nx = x > 0 ? x - 1 : 0; nx <= std::min(x + 1, w - 1); ++nx) {
                    cover[static_cast<std::size_t>(nz) * w + nx] = 1;
                }
            }
        }
    }
    auto surface = std::make_shared<WaterStrip>();
    std::vector<std::uint32_t> vertex(static_cast<std::size_t>(w + 1) * (h + 1), ~0u);
    const auto corner = [&](std::uint32_t x, std::uint32_t z) {
        std::uint32_t& v = vertex[static_cast<std::size_t>(z) * (w + 1) + x];
        if (v == ~0u) {
            v = static_cast<std::uint32_t>(surface->positions.size());
            surface->positions.emplace_back(origin.x + flood.origin.x + static_cast<float>(x) * flood.step, at.y,
                                            origin.z + flood.origin.y + static_cast<float>(z) * flood.step);
            surface->flow.emplace_back(0.0f);
        }
        return v;
    };
    for (std::uint32_t z = 0; z < h; ++z) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (!cover[static_cast<std::size_t>(z) * w + x]) {
                continue;
            }
            const std::uint32_t a = corner(x, z);
            const std::uint32_t b = corner(x, z + 1);
            const std::uint32_t c = corner(x + 1, z);
            const std::uint32_t d = corner(x + 1, z + 1);
            surface->indices.insert(surface->indices.end(), {a, b, c, c, b, d}); // counter-clockwise from above
        }
    }
    entry.surface = std::move(surface);
    return entry.surface;
}

} // namespace f3d::world
