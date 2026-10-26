// SPDX-License-Identifier: Apache-2.0
#include "f3d/world/scatter.hpp"

#include "f3d/core/jobs.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/world/scatter_paint.hpp"
#include "f3d/world/spline.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace f3d::world {
namespace {

constexpr float kTwoPi = 6.28318531f;
constexpr float kDegrees = 57.2957795f;

std::uint64_t mix(std::uint64_t x) noexcept { // splitmix64 finaliser
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// A uniform value in [0, 1) for candidate `index`: the same whatever else is drawn.
float random01(std::uint32_t seed, std::uint64_t index, std::uint32_t stream) noexcept {
    const std::uint64_t h = mix(mix(static_cast<std::uint64_t>(seed) + 0x9E3779B97F4A7C15ull) ^ (index * 8 + stream));
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

/// The weight (0-1) of `layer` at terrain-local (x, z): layer 0 covers what nothing was painted on.
float layer_weight(const terrain::Heightfield& heights, std::uint32_t layer, float x, float z) noexcept {
    const terrain::Splatmap* splat = heights.splat();
    if (!splat) {
        return layer == 0 ? 1.0f : 0.0f;
    }
    const std::uint32_t res = splat->resolution();
    const auto texel = [&](float v) {
        const float t = (v / heights.size() + 0.5f) * static_cast<float>(res);
        return static_cast<std::uint32_t>(std::clamp(t, 0.0f, static_cast<float>(res - 1)));
    };
    const std::uint32_t tx = texel(x);
    const std::uint32_t tz = texel(z);
    const std::uint32_t layers = splat->maps() * 4;
    if (layer >= layers) {
        return 0.0f;
    }
    if (layer == 0) {
        std::uint32_t sum = 0;
        for (std::uint32_t l = 0; l < layers; ++l) {
            sum += splat->weight(tx, tz, l);
        }
        if (sum == 0) {
            return 1.0f;
        }
    }
    return static_cast<float>(splat->weight(tx, tz, layer)) / 255.0f;
}

/// Rotation taking +Y to `up` (unit).
glm::quat tilt_to(const glm::vec3& up) noexcept {
    const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
    const float s = glm::length(axis);
    if (s < 1.0e-6f) {
        return up.y >= 0.0f ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : glm::angleAxis(3.14159265f, glm::vec3(1, 0, 0));
    }
    return glm::angleAxis(std::atan2(s, up.y), axis / s);
}

/// The settings that change the instances (not how they are drawn).
Scatter generation_key(Scatter s) {
    s.draw_distance = 0.0f;
    s.cast_shadows = false;
    s.visible = false;
    return s;
}

struct CacheEntry {
    Scatter key;
    glm::vec3 centre{0.0f};
    std::weak_ptr<terrain::Heightfield> heights;
    glm::vec3 origin{0.0f};
    std::uint64_t height_revision = 0;
    std::uint64_t splat_revision = 0;
    std::weak_ptr<ScatterMask> mask;
    std::uint64_t mask_revision = 0;
    std::uint64_t exclusion_revision = 0;
    bool valid = false;
    std::vector<ScatterInstance> instances;
};

/// Instances per scatter entity (registry context).
struct ScatterCache {
    std::unordered_map<entt::entity, CacheEntry> entries;
};

} // namespace

std::vector<ScatterInstance> generate_scatter(const Scatter& s, const glm::vec3& centre, const ScatterSurface& surface) {
    F3D_ZONE;
    std::vector<ScatterInstance> out;
    const glm::vec2 area = glm::max(s.area, glm::vec2(0.0f));
    if (s.meshes.empty() || s.max_instances == 0 || !(s.density > 0.0f) || area.x <= 0.0f || area.y <= 0.0f) {
        return out;
    }
    const double wanted = std::round(static_cast<double>(s.density) * area.x * area.y);
    const auto count = static_cast<std::size_t>(std::min(wanted, static_cast<double>(kMaxScatterCandidates)));

    // Placement and rules per candidate, in parallel: every candidate's random values
    // depend only on its index.
    struct Candidate {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        bool ok = false;
    };
    std::vector<Candidate> candidates(count);
    const terrain::Heightfield* heights = surface.heights;
    const bool layer_rule = heights && s.layer >= 0;
    const ScatterMask* mask = surface.mask;
    if (!mask && s.paint_only) {
        return out; // nothing painted yet
    }
    core::jobs::parallel_for(
        count,
        [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                Candidate& c = candidates[i];
                const float u = random01(s.seed, i, 0);
                const float v = random01(s.seed, i, 1);
                if (mask && random01(s.seed, i, 5) >= mask->sample(u, v)) {
                    continue; // painted out (or not painted in)
                }
                const float x = centre.x + (u - 0.5f) * area.x;
                const float z = centre.z + (v - 0.5f) * area.y;
                if (surface.excluded && surface.excluded(x, z)) {
                    continue;
                }
                float y = centre.y;
                glm::vec3 n(0.0f, 1.0f, 0.0f);
                if (heights) {
                    const float half = heights->size() * 0.5f;
                    const float lx = x - surface.origin.x;
                    const float lz = z - surface.origin.z;
                    if (std::abs(lx) > half || std::abs(lz) > half) {
                        continue; // off the terrain
                    }
                    y = heights->height_at(lx, lz) + surface.origin.y;
                    n = heights->normal_at(lx, lz);
                    if (layer_rule &&
                        layer_weight(*heights, static_cast<std::uint32_t>(s.layer), lx, lz) < s.layer_threshold) {
                        continue;
                    }
                }
                const float slope = std::acos(std::clamp(n.y, -1.0f, 1.0f)) * kDegrees;
                if (slope < s.slope_min || slope > s.slope_max || y < s.height_min || y > s.height_max) {
                    continue;
                }
                c = {glm::vec3(x, y, z), n, true};
            }
        },
        1024);

    // Minimum spacing, in index order (deterministic): a grid of cells at least
    // min_spacing wide, so any conflict lies in the 3 x 3 cells around a candidate.
    const float spacing = std::max(s.min_spacing, 0.0f);
    float cell = spacing > 0.0f ? spacing : 1.0f;
    const auto cells_along = [&](float extent) { return static_cast<std::size_t>(extent / cell) + 1; };
    while (spacing > 0.0f && cells_along(area.x) * cells_along(area.y) > (std::size_t{1} << 24)) {
        cell *= 2.0f;
    }
    const std::size_t nx = spacing > 0.0f ? cells_along(area.x) : 0;
    const std::size_t nz = spacing > 0.0f ? cells_along(area.y) : 0;
    std::vector<std::int32_t> head(nx * nz, -1);
    std::vector<std::int32_t> next;
    const glm::vec2 low = glm::vec2(centre.x, centre.z) - area * 0.5f;
    const auto cell_of = [&](float v, float lo, std::size_t n) {
        return static_cast<std::ptrdiff_t>(
            std::clamp((v - lo) / cell, 0.0f, static_cast<float>(n - 1)));
    };
    const float align = std::clamp(s.align_to_normal, 0.0f, 1.0f);
    const float spacing2 = spacing * spacing;
    const auto mesh_count = static_cast<std::uint32_t>(s.meshes.size());
    out.reserve(std::min<std::size_t>(count, s.max_instances));

    for (std::size_t i = 0; i < count && out.size() < s.max_instances; ++i) {
        const Candidate& c = candidates[i];
        if (!c.ok) {
            continue;
        }
        std::ptrdiff_t cx = 0;
        std::ptrdiff_t cz = 0;
        if (spacing > 0.0f) {
            cx = cell_of(c.position.x, low.x, nx);
            cz = cell_of(c.position.z, low.y, nz);
            bool clear = true;
            for (std::ptrdiff_t z = std::max<std::ptrdiff_t>(cz - 1, 0);
                 clear && z <= std::min<std::ptrdiff_t>(cz + 1, static_cast<std::ptrdiff_t>(nz) - 1); ++z) {
                for (std::ptrdiff_t x = std::max<std::ptrdiff_t>(cx - 1, 0);
                     clear && x <= std::min<std::ptrdiff_t>(cx + 1, static_cast<std::ptrdiff_t>(nx) - 1); ++x) {
                    for (std::int32_t k = head[static_cast<std::size_t>(z) * nx + static_cast<std::size_t>(x)];
                         k >= 0; k = next[static_cast<std::size_t>(k)]) {
                        const glm::vec4& p = out[static_cast<std::size_t>(k)].world[3];
                        const float dx = p.x - c.position.x;
                        const float dz = p.z - c.position.z;
                        if (dx * dx + dz * dz < spacing2) {
                            clear = false;
                            break;
                        }
                    }
                }
            }
            if (!clear) {
                continue;
            }
        }

        const float scale = s.scale_min + (s.scale_max - s.scale_min) * random01(s.seed, i, 2);
        const float yaw = s.random_yaw ? random01(s.seed, i, 3) * kTwoPi : 0.0f;
        const glm::vec3 up = glm::normalize(glm::mix(glm::vec3(0.0f, 1.0f, 0.0f), c.normal, align));
        const glm::quat rotation = tilt_to(up) * glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        // Sunk along the instance's up, keeping its exact ground point in x/z for the spacing test.
        const glm::vec3 position = c.position - glm::vec3(0.0f, s.sink * scale, 0.0f);
        ScatterInstance instance;
        instance.world = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *
                         glm::scale(glm::mat4(1.0f), glm::vec3(scale));
        instance.mesh = std::min(static_cast<std::uint32_t>(random01(s.seed, i, 4) * static_cast<float>(mesh_count)),
                                 mesh_count - 1);
        if (spacing > 0.0f) {
            const std::size_t slot = static_cast<std::size_t>(cz) * nx + static_cast<std::size_t>(cx);
            next.push_back(head[slot]);
            head[slot] = static_cast<std::int32_t>(out.size());
        }
        out.push_back(instance);
    }
    return out;
}

void register_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        entt::meta_factory<Scatter>{}
            .type("Scatter")
            .data<&Scatter::terrain>("terrain")
            .custom<FieldMeta>(FieldMeta{}.with_label("Surface").with_tooltip(
                "The terrain the instances stand on (pick it in the Scatter panel); "
                "none: flat ground at the entity's height"))
            .data<&Scatter::meshes>("meshes")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Meshes picked at random per instance (Scatter panel)"))
            .data<&Scatter::area>("area")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_tooltip(
                "Width (x) and depth (z) of the covered rectangle, centred on the entity"))
            .data<&Scatter::density>("density")
            .custom<FieldMeta>(FieldMeta{}.with_label("Amount").with_unit("/m2").with_range(0.0, 50.0, 0.001).with_tooltip(
                "How many instances are tried per square metre, before the rules and the spacing remove some "
                "(0.05: one per 20 m2)"))
            .data<&Scatter::min_spacing>("min_spacing")
            .custom<FieldMeta>(FieldMeta{}.with_label("Spacing").with_unit("m").with_range(0.0, 100.0, 0.05).with_tooltip(
                "Minimum distance between any two instances, so they don't overlap (0: no minimum)"))
            .data<&Scatter::scale_min>("scale_min")
            .custom<FieldMeta>(FieldMeta{}.with_label("Smallest size").with_range(0.01, 100.0, 0.01).with_tooltip(
                "Each instance gets a random size between the smallest and largest (1: the mesh's own size)"))
            .data<&Scatter::scale_max>("scale_max")
            .custom<FieldMeta>(FieldMeta{}.with_label("Largest size").with_range(0.01, 100.0, 0.01).with_tooltip(
                "Each instance gets a random size between the smallest and largest (1: the mesh's own size)"))
            .data<&Scatter::random_yaw>("random_yaw")
            .custom<FieldMeta>(FieldMeta{}.with_label("Random turn").with_tooltip(
                "Turn each instance to a random direction, so they don't all face the same way"))
            .data<&Scatter::align_to_normal>("align_to_normal")
            .custom<FieldMeta>(FieldMeta{}.with_label("Follow slope").with_range(0.0, 1.0, 0.01).with_tooltip(
                "Tilt with the ground: 0 stands upright (trees), 1 lies along the slope (rocks, grass)"))
            .data<&Scatter::sink>("sink")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(-10.0, 10.0, 0.01).with_tooltip(
                "Pushes instances into the ground (times their size), hiding floating edges on slopes"))
            .data<&Scatter::height_min>("height_min")
            .custom<FieldMeta>(FieldMeta{}.with_label("Lowest height").with_unit("m").with_tooltip(
                "Only place instances on ground at least this high (world height)"))
            .data<&Scatter::height_max>("height_max")
            .custom<FieldMeta>(FieldMeta{}.with_label("Highest height").with_unit("m").with_tooltip(
                "Only place instances on ground at most this high, e.g. a tree line (world height)"))
            .data<&Scatter::slope_min>("slope_min")
            .custom<FieldMeta>(FieldMeta{}.with_label("Least slope").with_unit("deg").with_range(0.0, 90.0, 0.5).with_tooltip(
                "Only place instances where the ground is at least this steep (0: flat)"))
            .data<&Scatter::slope_max>("slope_max")
            .custom<FieldMeta>(FieldMeta{}.with_label("Steepest slope").with_unit("deg").with_range(0.0, 90.0, 0.5).with_tooltip(
                "Only place instances where the ground is at most this steep (e.g. 30: no trees on cliffs)"))
            .data<&Scatter::layer>("layer")
            .custom<FieldMeta>(FieldMeta{}.with_label("Terrain layer").with_range(-1.0, 15.0, 1.0).with_tooltip(
                "Only place instances on this terrain layer (0: first layer, e.g. grass); -1: any ground"))
            .data<&Scatter::layer_threshold>("layer_threshold")
            .custom<FieldMeta>(FieldMeta{}.with_label("Layer coverage").with_range(0.0, 1.0, 0.01).with_tooltip(
                "How much the ground must be that layer (0.5: mostly; lower also uses blended edges)"))
            .data<&Scatter::seed>("seed")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Random layout number: change it for a different arrangement"))
            .data<&Scatter::max_instances>("max_instances")
            .custom<FieldMeta>(FieldMeta{}.with_label("Instance limit").with_range(0.0, 1.0e6, 100.0).with_tooltip(
                "Upper bound on the number of instances, to protect performance"))
            .data<&Scatter::draw_distance>("draw_distance")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(1.0, 1.0e5, 1.0).with_tooltip(
                "Instances further than this from the camera are not drawn"))
            .data<&Scatter::cast_shadows>("cast_shadows")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Instances cast shadows (off: faster)"))
            .data<&Scatter::visible>("visible")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Draw the instances"))
            .data<&Scatter::paint_only>("paint_only")
            .custom<FieldMeta>(FieldMeta{}.with_label("Painted only").with_tooltip(
                "Place instances only where painted with the Foliage brush (off: everywhere the rules allow, "
                "until erased)"));
        scene::register_component<Scatter>("Scatter", true);
        return true;
    }();
    static_cast<void>(registered);
}

const std::vector<ScatterInstance>& scatter_instances(scene::World& world, entt::entity e) {
    static const std::vector<ScatterInstance> kNone;
    entt::registry& registry = world.registry();
    const Scatter* scatter = world.valid(e) ? registry.try_get<Scatter>(e) : nullptr;
    if (!scatter) {
        return kNone;
    }
    world.update_transforms();
    const glm::vec3 centre(world.transform(e).world[3]);
    ScatterSurface surface;
    std::shared_ptr<terrain::Heightfield> heights;
    if (!scatter->terrain.is_null()) {
        const entt::entity t = world.find(scatter->terrain);
        heights = terrain::surface_of(world, t); // with roads and riverbeds
        if (heights) {
            surface.heights = heights.get();
            surface.terrain = &registry.get<terrain::Terrain>(t);
            surface.origin = terrain::terrain_origin(world, t);
        }
    }
    const std::uint64_t height_revision = heights ? heights->revision() : 0;
    const std::uint64_t splat_revision = heights && heights->splat() ? heights->splat()->revision() : 0;
    const std::shared_ptr<ScatterMask> mask = scatter_masks(world).find(world.guid(e));
    surface.mask = mask.get();
    const std::uint64_t mask_revision = mask ? mask->revision() : 0;
    std::uint64_t exclusion_revision = 0;
    surface.excluded = scatter_exclusion(world, *scatter, exclusion_revision);

    if (!registry.ctx().contains<ScatterCache>()) {
        registry.ctx().emplace<ScatterCache>();
    }
    CacheEntry& entry = registry.ctx().get<ScatterCache>().entries[e];
    Scatter key = generation_key(*scatter);
    if (!entry.valid || !(entry.key == key) || entry.centre != centre || entry.heights.lock() != heights ||
        entry.origin != surface.origin || entry.height_revision != height_revision ||
        entry.splat_revision != splat_revision || entry.mask.lock() != mask || entry.mask_revision != mask_revision ||
        entry.exclusion_revision != exclusion_revision) {
        entry.instances = generate_scatter(*scatter, centre, surface);
        entry.key = std::move(key);
        entry.centre = centre;
        entry.heights = heights;
        entry.origin = surface.origin;
        entry.height_revision = height_revision;
        entry.splat_revision = splat_revision;
        entry.mask = mask;
        entry.mask_revision = mask_revision;
        entry.exclusion_revision = exclusion_revision;
        entry.valid = true;
    }
    return entry.instances;
}

void prune_scatter_cache(scene::World& world) {
    entt::registry& registry = world.registry();
    if (!registry.ctx().contains<ScatterCache>()) {
        return;
    }
    std::erase_if(registry.ctx().get<ScatterCache>().entries, [&](const auto& entry) {
        return !world.valid(entry.first) || !registry.all_of<Scatter>(entry.first);
    });
}

const std::vector<ScatterPreset>& scatter_presets() {
    static const std::vector<ScatterPreset> presets = [] {
        std::vector<ScatterPreset> out;
        Scatter trees;
        trees.meshes = {{core::Guid{}, kBuiltinPine}, {core::Guid{}, kBuiltinBroadleaf}};
        trees.density = 0.01f;
        trees.min_spacing = 5.0f;
        trees.scale_min = 0.7f;
        trees.scale_max = 1.4f;
        trees.sink = 0.3f;
        trees.slope_max = 30.0f;
        trees.layer = 0; // the grass layer of default_layers()
        trees.max_instances = 50000;
        trees.draw_distance = 1500.0f;
        out.push_back({"trees", "Trees", trees});

        Scatter grass;
        grass.meshes = {{core::Guid{}, kBuiltinGrass}};
        grass.area = {96.0f, 96.0f};
        grass.density = 1.0f;
        grass.min_spacing = 0.5f;
        grass.scale_min = 0.6f;
        grass.scale_max = 1.3f;
        grass.align_to_normal = 0.6f;
        grass.slope_max = 35.0f;
        grass.layer = 0;
        grass.max_instances = 50000;
        grass.draw_distance = 120.0f;
        grass.cast_shadows = false;
        out.push_back({"grass", "Grass", grass, false}); // dense: a patch, not a whole terrain

        Scatter rocks;
        rocks.meshes = {{core::Guid{}, kBuiltinRock}};
        rocks.density = 0.003f;
        rocks.min_spacing = 3.0f;
        rocks.scale_min = 0.4f;
        rocks.scale_max = 2.5f;
        rocks.align_to_normal = 0.7f;
        rocks.sink = 0.15f;
        rocks.max_instances = 20000;
        rocks.draw_distance = 800.0f;
        out.push_back({"rocks", "Rocks", rocks});
        return out;
    }();
    return presets;
}

} // namespace f3d::world
