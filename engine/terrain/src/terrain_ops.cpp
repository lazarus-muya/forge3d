// SPDX-License-Identifier: Apache-2.0
#include "f3d/terrain/terrain_ops.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/ops/operation.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/builtin_textures.hpp"
#include "f3d/terrain/erosion.hpp"
#include "f3d/terrain/layers.hpp"
#include "f3d/terrain/sculpt.hpp"
#include "f3d/terrain/terrain.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>
#include <new>
#include <optional>
#include <vector>

namespace f3d::terrain {
namespace {

// Globally unique names: EnTT identifies types by name (see prefab_ops.cpp).
struct TerrainCreateOpParams {
    std::string name = "Terrain";
    core::Guid guid;
    std::uint32_t tiles = 4;
    float size = 1024.0f;
    float min_height = 0.0f;
    float max_height = 256.0f;
    std::string format = "r16";
    std::uint32_t seed = 1;
    float frequency = 4.0f;
    std::uint32_t octaves = 7;
};

struct TerrainSculptStrokeOpParams {
    core::Guid terrain;
    std::string tool = "raise";
    float radius = 32.0f;
    float strength = 0.5f;
    std::string falloff = "smooth";
    float spacing = 0.25f;
    bool use_target = false;
    float target_height = 0.0f;
    float terrace_step = 8.0f;
    float noise_scale = 16.0f;
    std::uint32_t noise_seed = 1;
    std::string stamp;
    float stamp_height = 16.0f;
    float stamp_rotation = 0.0f;
    std::uint32_t erosion_iterations = 12;
    std::vector<glm::vec3> points; ///< x, z (terrain-local metres), pressure
};

struct TerrainApplyRulesOpParams {
    core::Guid terrain;
};

struct TerrainSetLayersOpParams {
    core::Guid terrain;
    std::vector<TerrainLayer> layers;
    std::vector<std::int32_t> source; ///< per new layer: its old index (-1: new); empty: unchanged order
};

struct TerrainAutoMaterialOpParams {
    core::Guid terrain;
    std::string preset = "grassland";
};

struct TerrainPaintStrokeOpParams {
    core::Guid terrain;
    std::uint32_t layer = 0;
    bool erase = false;
    float radius = 16.0f;
    float strength = 0.5f;
    std::string falloff = "smooth";
    float spacing = 0.25f;
    std::vector<glm::vec3> points; ///< x, z (terrain-local metres), pressure
};

struct TerrainErodeOpParams {
    core::Guid terrain;
    std::string mode = "both";
    std::uint32_t iterations = 200;
    std::uint32_t seed = 1;
    float rain = 0.012f;
    float evaporation = 0.015f;
    float capacity = 1.0f;
    float dissolving = 0.3f;
    float deposition = 0.3f;
    float min_slope = 0.05f;
    float talus_angle = 35.0f;
    float thermal_rate = 0.25f;
};

void register_param_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        entt::meta_factory<TerrainCreateOpParams>{}
            .type("terrain.create.params")
            .data<&TerrainCreateOpParams::name>("name")
            .data<&TerrainCreateOpParams::guid>("guid")
            .data<&TerrainCreateOpParams::tiles>("tiles")
            .custom<FieldMeta>(FieldMeta{}.with_range(1.0, 64.0, 1.0).with_tooltip("256-quad tiles per side (32: 8k)"))
            .data<&TerrainCreateOpParams::size>("size")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(1.0, 1.0e5, 1.0))
            .data<&TerrainCreateOpParams::min_height>("min_height")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m"))
            .data<&TerrainCreateOpParams::max_height>("max_height")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m"))
            .data<&TerrainCreateOpParams::format>("format")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("r16 or r32f"))
            .data<&TerrainCreateOpParams::seed>("seed")
            .data<&TerrainCreateOpParams::frequency>("frequency")
            .custom<FieldMeta>(
                FieldMeta{}.with_range(0.0, 256.0, 0.1).with_tooltip("Noise features across the terrain"))
            .data<&TerrainCreateOpParams::octaves>("octaves")
            .custom<FieldMeta>(FieldMeta{}.with_range(1.0, 16.0, 1.0));
        entt::meta_factory<TerrainSculptStrokeOpParams>{}
            .type("terrain.sculpt_stroke.params")
            .data<&TerrainSculptStrokeOpParams::terrain>("terrain")
            .data<&TerrainSculptStrokeOpParams::tool>("tool")
            .custom<FieldMeta>(
                FieldMeta{}.with_tooltip("raise, lower, smooth, flatten, terrace, noise, stamp or erode"))
            .data<&TerrainSculptStrokeOpParams::radius>("radius")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 1.0e4, 0.1))
            .data<&TerrainSculptStrokeOpParams::strength>("strength")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01))
            .data<&TerrainSculptStrokeOpParams::falloff>("falloff")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("smooth, linear, sphere or constant"))
            .data<&TerrainSculptStrokeOpParams::spacing>("spacing")
            .custom<FieldMeta>(
                FieldMeta{}.with_range(0.01, 4.0, 0.01).with_tooltip("Dab spacing, fraction of the radius"))
            .data<&TerrainSculptStrokeOpParams::use_target>("use_target")
            .data<&TerrainSculptStrokeOpParams::target_height>("target_height")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m"))
            .data<&TerrainSculptStrokeOpParams::terrace_step>("terrace_step")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m"))
            .data<&TerrainSculptStrokeOpParams::noise_scale>("noise_scale")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m"))
            .data<&TerrainSculptStrokeOpParams::noise_seed>("noise_seed")
            .data<&TerrainSculptStrokeOpParams::stamp>("stamp")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Heightmap image for the stamp tool"))
            .data<&TerrainSculptStrokeOpParams::stamp_height>("stamp_height")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m"))
            .data<&TerrainSculptStrokeOpParams::stamp_rotation>("stamp_rotation")
            .custom<FieldMeta>(FieldMeta{}.with_unit("rad"))
            .data<&TerrainSculptStrokeOpParams::erosion_iterations>("erosion_iterations")
            .custom<FieldMeta>(FieldMeta{}.with_range(1.0, 200.0, 1.0).with_tooltip("Erode: iterations per dab"))
            .data<&TerrainSculptStrokeOpParams::points>("points")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("[x, z, pressure] per point, terrain-local metres"));
        entt::meta_factory<TerrainApplyRulesOpParams>{}
            .type("terrain.apply_rules.params")
            .data<&TerrainApplyRulesOpParams::terrain>("terrain");
        entt::meta_factory<TerrainPaintStrokeOpParams>{}
            .type("terrain.paint_stroke.params")
            .data<&TerrainPaintStrokeOpParams::terrain>("terrain")
            .data<&TerrainPaintStrokeOpParams::layer>("layer")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 15.0, 1.0).with_tooltip("Index in Terrain.layers"))
            .data<&TerrainPaintStrokeOpParams::erase>("erase")
            .data<&TerrainPaintStrokeOpParams::radius>("radius")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 1.0e4, 0.1))
            .data<&TerrainPaintStrokeOpParams::strength>("strength")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01))
            .data<&TerrainPaintStrokeOpParams::falloff>("falloff")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("smooth, linear, sphere or constant"))
            .data<&TerrainPaintStrokeOpParams::spacing>("spacing")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.01, 4.0, 0.01))
            .data<&TerrainPaintStrokeOpParams::points>("points")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("[x, z, pressure] per point, terrain-local metres"));
        entt::meta_factory<TerrainSetLayersOpParams>{}
            .type("terrain.set_layers.params")
            .data<&TerrainSetLayersOpParams::terrain>("terrain")
            .data<&TerrainSetLayersOpParams::layers>("layers")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("The new Terrain.layers, bottom to top (at most 16)"))
            .data<&TerrainSetLayersOpParams::source>("source")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip(
                "Per new layer, the index it had before (-1: new), so painted weights follow; empty: same order"));
        entt::meta_factory<TerrainAutoMaterialOpParams>{}
            .type("terrain.auto_material.params")
            .data<&TerrainAutoMaterialOpParams::terrain>("terrain")
            .data<&TerrainAutoMaterialOpParams::preset>("preset")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("grassland, alpine, desert, forest or coast"));
        entt::meta_factory<TerrainErodeOpParams>{}
            .type("terrain.erode.params")
            .data<&TerrainErodeOpParams::terrain>("terrain")
            .data<&TerrainErodeOpParams::mode>("mode")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("hydraulic, thermal or both"))
            .data<&TerrainErodeOpParams::iterations>("iterations")
            .custom<FieldMeta>(FieldMeta{}.with_range(1.0, 5000.0, 1.0))
            .data<&TerrainErodeOpParams::seed>("seed")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Rain pattern"))
            .data<&TerrainErodeOpParams::rain>("rain")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 0.2, 0.001).with_tooltip("Water per iteration"))
            .data<&TerrainErodeOpParams::evaporation>("evaporation")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.001))
            .data<&TerrainErodeOpParams::capacity>("capacity")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 10.0, 0.01).with_tooltip("Sediment the water carries"))
            .data<&TerrainErodeOpParams::dissolving>("dissolving")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01))
            .data<&TerrainErodeOpParams::deposition>("deposition")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01))
            .data<&TerrainErodeOpParams::min_slope>("min_slope")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01))
            .data<&TerrainErodeOpParams::talus_angle>("talus_angle")
            .custom<FieldMeta>(FieldMeta{}.with_unit("deg").with_range(0.0, 89.0, 0.5))
            .data<&TerrainErodeOpParams::thermal_rate>("thermal_rate")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01));
        return true;
    }();
    static_cast<void>(registered);
}

class TerrainCreateOp final : public ops::OperationBase<TerrainCreateOpParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "terrain.create"; }
    [[nodiscard]] std::string display_name() const override { return "Create terrain " + params.name; }

    bool execute(ops::Context& context) override {
        if (params.tiles < 1 || params.tiles > 64 || params.size <= 0.0f || params.max_height < params.min_height ||
            (params.format != "r16" && params.format != "r32f")) {
            F3D_LOG_ERROR("terrain.create: tiles 1..64, size > 0, max_height >= min_height, format r16 or r32f");
            return false;
        }
        if (params.guid.is_null()) {
            params.guid = core::Guid::generate(); // fixed now, so redo recreates the same entity
        }
        scene::World& world = context.world;
        if (world.find(params.guid) != entt::null) {
            return false;
        }
        const entt::entity e = world.create(params.name, entt::null, params.guid);
        if (e == entt::null) {
            return false;
        }
        Terrain terrain;
        terrain.tiles = params.tiles;
        terrain.size = params.size;
        terrain.min_height = params.min_height;
        terrain.max_height = params.max_height;
        terrain.format = params.format == "r32f" ? HeightFormat::R32F : HeightFormat::R16;
        terrain.layers = default_layers(params.min_height, params.max_height);
        world.registry().emplace<Terrain>(e, terrain);
        if (!heights_) {
            F3D_ZONE_N("Generate terrain");
            heights_ = std::make_shared<Heightfield>(params.tiles, params.size);
            NoiseSettings noise;
            noise.seed = params.seed;
            noise.frequency = params.frequency;
            noise.octaves = std::clamp(params.octaves, 1u, 16u);
            generate_noise(*heights_, noise, params.min_height, params.max_height);
            apply_rules(*heights_, terrain, heights_->ensure_splat(static_cast<std::uint32_t>(terrain.layers.size())));
        }
        heights_->mark_all_dirty();
        heightfields(world).set(params.guid, heights_);
        context.changes.emit({ops::ChangeKind::EntityCreated, params.guid, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        scene::World& world = context.world;
        heightfields(world).erase(params.guid);
        const entt::entity e = world.find(params.guid);
        if (e != entt::null) {
            world.destroy(e);
        }
        context.changes.emit({ops::ChangeKind::EntityDestroyed, params.guid, {}});
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (heights_ ? heights_->heights().size() * sizeof(float) : 0);
    }

private:
    std::shared_ptr<Heightfield> heights_; ///< kept for redo
};

class TerrainSculptStrokeOp final : public ops::OperationBase<TerrainSculptStrokeOpParams> {
public:
    TerrainSculptStrokeOp() = default;
    explicit TerrainSculptStrokeOp(std::unique_ptr<StrokeRecorder> applied) : applied_(std::move(applied)) {}

    [[nodiscard]] std::string_view id() const override { return "terrain.sculpt_stroke"; }
    [[nodiscard]] std::string display_name() const override { return "Sculpt (" + params.tool + ")"; }

    bool execute(ops::Context& context) override {
        scene::World& world = context.world;
        const entt::entity e = world.find(params.terrain);
        const Terrain* terrain = e == entt::null ? nullptr : world.registry().try_get<Terrain>(e);
        const std::shared_ptr<Heightfield> hf = terrain ? heightfield_of(world, e) : nullptr;
        if (!terrain || !hf) {
            F3D_LOG_ERROR("terrain.sculpt_stroke: {} is not a terrain", params.terrain.to_string());
            return false;
        }
        if (diff_) { // redo
            diff_->apply_after(*hf);
        } else if (applied_ && applied_->heightfield() == hf) { // applied live by the editor
            diff_ = applied_->finish();
            applied_.reset();
        } else {
            Brush brush;
            if (!make_brush(brush)) {
                return false;
            }
            std::shared_ptr<const Stamp> stamp;
            if (brush.tool == BrushTool::Stamp) {
                stamp = load_stamp(brush.stamp, context.files);
                if (!stamp) {
                    F3D_LOG_ERROR("terrain.sculpt_stroke: cannot read the stamp '{}'", brush.stamp);
                    return false;
                }
            }
            F3D_ZONE_N("Sculpt stroke");
            StrokeRecorder stroke(hf, *terrain, brush, std::move(stamp));
            for (const glm::vec3& p : params.points) {
                stroke.add({{p.x, p.y}, p.z});
            }
            diff_ = stroke.finish();
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        scene::World& world = context.world;
        const entt::entity e = world.find(params.terrain);
        const std::shared_ptr<Heightfield> hf = e == entt::null ? nullptr : heightfield_of(world, e);
        if (hf && diff_) {
            diff_->apply_before(*hf);
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + params.points.size() * sizeof(glm::vec3) + (diff_ ? diff_->bytes() : 0);
    }

private:
    bool make_brush(Brush& brush) const {
        if (!parse(params.tool, brush.tool) || !parse(params.falloff, brush.falloff)) {
            F3D_LOG_ERROR("terrain.sculpt_stroke: unknown tool '{}' or falloff '{}'", params.tool, params.falloff);
            return false;
        }
        if (!(params.radius > 0.0f) || params.points.empty()) {
            F3D_LOG_ERROR("terrain.sculpt_stroke: needs radius > 0 and at least one point");
            return false;
        }
        brush.radius = params.radius;
        brush.strength = params.strength;
        brush.spacing = params.spacing;
        brush.use_target = params.use_target;
        brush.target_height = params.target_height;
        brush.terrace_step = params.terrace_step;
        brush.noise_scale = params.noise_scale;
        brush.noise_seed = params.noise_seed;
        brush.stamp = params.stamp;
        brush.stamp_height = params.stamp_height;
        brush.stamp_rotation = params.stamp_rotation;
        brush.erosion_iterations = params.erosion_iterations;
        return true;
    }

    std::optional<TileDiff> diff_;
    std::unique_ptr<StrokeRecorder> applied_;
};

/// The terrain entity, component and heights of `guid`, or nulls (logged as `op`).
struct TerrainTarget {
    const Terrain* terrain = nullptr;
    std::shared_ptr<Heightfield> heights;
};
TerrainTarget find_terrain(scene::World& world, const core::Guid& guid, std::string_view op) {
    const entt::entity e = world.find(guid);
    TerrainTarget out;
    out.terrain = e == entt::null ? nullptr : world.registry().try_get<Terrain>(e);
    out.heights = out.terrain ? heightfield_of(world, e) : nullptr;
    if (!out.heights) {
        F3D_LOG_ERROR("{}: {} is not a terrain", op, guid.to_string());
        out.terrain = nullptr;
    }
    return out;
}

class TerrainApplyRulesOp final : public ops::OperationBase<TerrainApplyRulesOpParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "terrain.apply_rules"; }
    [[nodiscard]] std::string display_name() const override { return "Apply layer rules"; }

    bool execute(ops::Context& context) override {
        const TerrainTarget target = find_terrain(context.world, params.terrain, "terrain.apply_rules");
        if (!target.terrain) {
            return false;
        }
        Splatmap& splat =
            target.heights->ensure_splat(static_cast<std::uint32_t>(std::max<std::size_t>(target.terrain->layers.size(), 1)));
        if (diff_) {
            diff_->apply_after(splat);
        } else {
            const auto before = splat_blocks(splat);
            apply_rules(*target.heights, *target.terrain, splat);
            diff_ = diff_splat(splat, before);
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        const TerrainTarget target = find_terrain(context.world, params.terrain, "terrain.apply_rules");
        if (target.terrain && target.heights->splat() && diff_) {
            diff_->apply_before(*target.heights->splat());
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
    }

    [[nodiscard]] std::size_t memory_size() const override { return sizeof(*this) + (diff_ ? diff_->bytes() : 0); }

private:
    std::optional<SplatDiff> diff_;
};

class TerrainSetLayersOp final : public ops::OperationBase<TerrainSetLayersOpParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "terrain.set_layers"; }
    [[nodiscard]] std::string display_name() const override { return "Edit terrain layers"; }

    bool execute(ops::Context& context) override {
        const TerrainTarget target = find_terrain(context.world, params.terrain, "terrain.set_layers");
        if (!target.terrain) {
            return false;
        }
        if (params.layers.empty() || params.layers.size() > kMaxLayers ||
            (!params.source.empty() && params.source.size() != params.layers.size())) {
            F3D_LOG_ERROR("terrain.set_layers: needs 1 to 16 layers and, if given, one source index per layer");
            return false;
        }
        Terrain& terrain = context.world.registry().get<Terrain>(context.world.find(params.terrain));
        if (!has_before_) {
            before_ = terrain.layers;
            has_before_ = true;
        }
        terrain.layers = params.layers;
        if (!params.source.empty() && target.heights->splat()) {
            Splatmap& splat = *target.heights->splat();
            if (diff_) {
                diff_->apply_after(splat);
            } else {
                const auto blocks = splat_blocks(splat);
                remap_splat(splat, params.source);
                diff_ = diff_splat(splat, blocks);
            }
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        const entt::entity e = context.world.find(params.terrain);
        if (e == entt::null || !has_before_ || !context.world.registry().all_of<Terrain>(e)) {
            return;
        }
        context.world.registry().get<Terrain>(e).layers = before_;
        const auto heights = heightfield_of(context.world, e);
        if (diff_ && heights && heights->splat()) {
            diff_->apply_before(*heights->splat());
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
    }

    bool merge(const Operation& next) override {
        // A continuing edit (a slider drag) of the same terrain without reordering.
        const auto* other = dynamic_cast<const TerrainSetLayersOp*>(&next);
        if (!other || other->params.terrain != params.terrain || !params.source.empty() ||
            !other->params.source.empty()) {
            return false;
        }
        params.layers = other->params.layers;
        return true;
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (diff_ ? diff_->bytes() : 0) +
               (before_.size() + params.layers.size()) * sizeof(TerrainLayer);
    }

private:
    std::vector<TerrainLayer> before_;
    bool has_before_ = false;
    std::optional<SplatDiff> diff_;
};

class TerrainAutoMaterialOp final : public ops::OperationBase<TerrainAutoMaterialOpParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "terrain.auto_material"; }
    [[nodiscard]] std::string display_name() const override { return "Auto material: " + params.preset; }

    bool execute(ops::Context& context) override {
        const TerrainTarget target = find_terrain(context.world, params.terrain, "terrain.auto_material");
        if (!target.terrain) {
            return false;
        }
        Terrain& terrain = context.world.registry().get<Terrain>(context.world.find(params.terrain));
        if (!diff_) {
            const glm::vec2 range = target.heights->range();
            after_ = landscape_preset_layers(params.preset, range.x, range.y);
            if (after_.empty()) {
                F3D_LOG_ERROR("terrain.auto_material: unknown preset '{}'", params.preset);
                return false;
            }
            before_ = terrain.layers;
        }
        terrain.layers = after_;
        Splatmap& splat = target.heights->ensure_splat(static_cast<std::uint32_t>(after_.size()));
        if (diff_) {
            diff_->apply_after(splat);
        } else {
            const auto before = splat_blocks(splat);
            apply_rules(*target.heights, terrain, splat);
            diff_ = diff_splat(splat, before);
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        const TerrainTarget target = find_terrain(context.world, params.terrain, "terrain.auto_material");
        if (!target.terrain || !diff_) {
            return;
        }
        context.world.registry().get<Terrain>(context.world.find(params.terrain)).layers = before_;
        if (target.heights->splat()) {
            diff_->apply_before(*target.heights->splat());
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (diff_ ? diff_->bytes() : 0) + (before_.size() + after_.size()) * sizeof(TerrainLayer);
    }

private:
    std::vector<TerrainLayer> before_;
    std::vector<TerrainLayer> after_;
    std::optional<SplatDiff> diff_;
};

class TerrainPaintStrokeOp final : public ops::OperationBase<TerrainPaintStrokeOpParams> {
public:
    TerrainPaintStrokeOp() = default;
    explicit TerrainPaintStrokeOp(std::unique_ptr<PaintRecorder> applied) : applied_(std::move(applied)) {}

    [[nodiscard]] std::string_view id() const override { return "terrain.paint_stroke"; }
    [[nodiscard]] std::string display_name() const override {
        return std::string(params.erase ? "Erase" : "Paint") + " layer " + std::to_string(params.layer);
    }

    bool execute(ops::Context& context) override {
        const TerrainTarget target = find_terrain(context.world, params.terrain, "terrain.paint_stroke");
        if (!target.terrain) {
            return false;
        }
        if (diff_) {
            diff_->apply_after(target.heights->ensure_splat(diff_->maps * 4));
        } else if (applied_ && applied_->heightfield() == target.heights) {
            diff_ = applied_->finish();
            applied_.reset();
        } else {
            Brush brush;
            if (!parse(params.falloff, brush.falloff)) {
                F3D_LOG_ERROR("terrain.paint_stroke: unknown falloff '{}'", params.falloff);
                return false;
            }
            if (params.layer >= kMaxLayers || !(params.radius > 0.0f) || params.points.empty()) {
                F3D_LOG_ERROR("terrain.paint_stroke: needs a layer 0..15, radius > 0 and at least one point");
                return false;
            }
            brush.radius = params.radius;
            brush.strength = params.strength;
            brush.spacing = params.spacing;
            PaintRecorder stroke(target.heights, *target.terrain, brush, params.layer, params.erase);
            for (const glm::vec3& p : params.points) {
                stroke.add({{p.x, p.y}, p.z});
            }
            diff_ = stroke.finish();
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        const TerrainTarget target = find_terrain(context.world, params.terrain, "terrain.paint_stroke");
        if (target.terrain && target.heights->splat() && diff_) {
            diff_->apply_before(*target.heights->splat());
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + params.points.size() * sizeof(glm::vec3) + (diff_ ? diff_->bytes() : 0);
    }

private:
    std::optional<SplatDiff> diff_;
    std::unique_ptr<PaintRecorder> applied_;
};

TerrainErodeOpParams params_from(const core::Guid& terrain, const ErosionSettings& s) {
    TerrainErodeOpParams p;
    p.terrain = terrain;
    p.mode = std::string(to_string(s.mode));
    p.iterations = s.iterations;
    p.seed = s.seed;
    p.rain = s.rain;
    p.evaporation = s.evaporation;
    p.capacity = s.capacity;
    p.dissolving = s.dissolving;
    p.deposition = s.deposition;
    p.min_slope = s.min_slope;
    p.talus_angle = s.talus_angle;
    p.thermal_rate = s.thermal_rate;
    return p;
}

class TerrainErodeOp final : public ops::OperationBase<TerrainErodeOpParams> {
public:
    TerrainErodeOp() = default;
    TerrainErodeOp(std::vector<float> result, std::uint64_t revision)
        : precomputed_(std::move(result)), precomputed_revision_(revision) {}

    [[nodiscard]] std::string_view id() const override { return "terrain.erode"; }
    [[nodiscard]] std::string display_name() const override { return "Erode (" + params.mode + ")"; }

    bool execute(ops::Context& context) override {
        scene::World& world = context.world;
        const entt::entity e = world.find(params.terrain);
        const Terrain* terrain = e == entt::null ? nullptr : world.registry().try_get<Terrain>(e);
        const std::shared_ptr<Heightfield> hf = terrain ? heightfield_of(world, e) : nullptr;
        if (!terrain || !hf) {
            F3D_LOG_ERROR("terrain.erode: {} is not a terrain", params.terrain.to_string());
            return false;
        }
        if (!diff_) {
            ErosionSettings settings;
            if (!make_settings(settings)) {
                return false;
            }
            std::vector<float> after;
            if (!precomputed_.empty() && precomputed_revision_ == hf->revision() &&
                precomputed_.size() == hf->heights().size()) {
                after = std::move(precomputed_);
            } else {
                after = hf->heights();
                try {
                    F3D_ZONE_N("Erode terrain");
                    if (!erode(after, hf->samples(), hf->samples(), hf->spacing(), settings, terrain->min_height,
                               terrain->max_height, context.progress)) {
                        F3D_LOG_INFO("terrain.erode: cancelled");
                        return false;
                    }
                } catch (const std::bad_alloc&) {
                    F3D_LOG_ERROR("terrain.erode: not enough memory ({} MB of scratch)",
                                  erosion_memory(hf->samples(), hf->samples()) >> 20);
                    return false;
                }
            }
            precomputed_.clear();
            precomputed_.shrink_to_fit();
            diff_ = diff_tiles(*hf, hf->heights(), after);
        }
        diff_->apply_after(*hf);
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        scene::World& world = context.world;
        const entt::entity e = world.find(params.terrain);
        const std::shared_ptr<Heightfield> hf = e == entt::null ? nullptr : heightfield_of(world, e);
        if (hf && diff_) {
            diff_->apply_before(*hf);
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (diff_ ? diff_->bytes() : 0) + precomputed_.size() * sizeof(float);
    }

private:
    bool make_settings(ErosionSettings& s) const {
        if (!parse(params.mode, s.mode)) {
            F3D_LOG_ERROR("terrain.erode: unknown mode '{}' (hydraulic, thermal or both)", params.mode);
            return false;
        }
        if (params.iterations < 1 || params.iterations > 100000) {
            F3D_LOG_ERROR("terrain.erode: iterations must be 1..100000");
            return false;
        }
        s.iterations = params.iterations;
        s.seed = params.seed;
        s.rain = params.rain;
        s.evaporation = params.evaporation;
        s.capacity = params.capacity;
        s.dissolving = params.dissolving;
        s.deposition = params.deposition;
        s.min_slope = params.min_slope;
        s.talus_angle = params.talus_angle;
        s.thermal_rate = params.thermal_rate;
        return true;
    }

    std::optional<TileDiff> diff_;
    std::vector<float> precomputed_;
    std::uint64_t precomputed_revision_ = 0;
};

} // namespace

std::unique_ptr<ops::Operation> make_erode_op(const core::Guid& terrain, const ErosionSettings& settings,
                                              std::vector<float> result, std::uint64_t revision) {
    register_param_types();
    auto op = std::make_unique<TerrainErodeOp>(std::move(result), revision);
    op->params = params_from(terrain, settings);
    return op;
}

std::unique_ptr<ops::Operation> make_paint_stroke_op(const core::Guid& terrain, std::unique_ptr<PaintRecorder> applied) {
    register_param_types();
    TerrainPaintStrokeOpParams p;
    p.terrain = terrain;
    p.layer = applied->layer();
    p.erase = applied->erase();
    p.radius = applied->brush().radius;
    p.strength = applied->brush().strength;
    p.falloff = std::string(to_string(applied->brush().falloff));
    p.spacing = applied->brush().spacing;
    for (const StrokePoint& point : applied->points()) {
        p.points.emplace_back(point.position.x, point.position.y, point.pressure);
    }
    auto op = std::make_unique<TerrainPaintStrokeOp>(std::move(applied));
    op->params = std::move(p);
    return op;
}

std::unique_ptr<ops::Operation> make_sculpt_stroke_op(const core::Guid& terrain,
                                                      std::unique_ptr<StrokeRecorder> applied) {
    register_param_types();
    TerrainSculptStrokeOpParams p;
    const Brush& brush = applied->brush();
    p.terrain = terrain;
    p.tool = std::string(to_string(brush.tool));
    p.radius = brush.radius;
    p.strength = brush.strength;
    p.falloff = std::string(to_string(brush.falloff));
    p.spacing = brush.spacing;
    p.use_target = brush.use_target;
    p.target_height = brush.target_height;
    p.terrace_step = brush.terrace_step;
    p.noise_scale = brush.noise_scale;
    p.noise_seed = brush.noise_seed;
    p.stamp = brush.stamp;
    p.stamp_height = brush.stamp_height;
    p.stamp_rotation = brush.stamp_rotation;
    p.erosion_iterations = brush.erosion_iterations;
    for (const StrokePoint& point : applied->points()) {
        p.points.emplace_back(point.position.x, point.position.y, point.pressure);
    }
    auto op = std::make_unique<TerrainSculptStrokeOp>(std::move(applied));
    op->params = std::move(p);
    return op;
}

void register_terrain_ops(ops::OpRegistry& registry) {
    register_types();
    register_param_types();
    registry.add({"terrain.create", "Create Terrain", entt::resolve<TerrainCreateOpParams>()},
                 [] { return std::make_unique<TerrainCreateOp>(); });
    registry.add({"terrain.sculpt_stroke", "Sculpt Terrain", entt::resolve<TerrainSculptStrokeOpParams>()},
                 [] { return std::make_unique<TerrainSculptStrokeOp>(); });
    registry.add({"terrain.erode", "Erode Terrain", entt::resolve<TerrainErodeOpParams>()},
                 [] { return std::make_unique<TerrainErodeOp>(); });
    registry.add({"terrain.apply_rules", "Apply Layer Rules", entt::resolve<TerrainApplyRulesOpParams>()},
                 [] { return std::make_unique<TerrainApplyRulesOp>(); });
    registry.add({"terrain.set_layers", "Edit Terrain Layers", entt::resolve<TerrainSetLayersOpParams>()},
                 [] { return std::make_unique<TerrainSetLayersOp>(); });
    registry.add({"terrain.auto_material", "Auto Material", entt::resolve<TerrainAutoMaterialOpParams>()},
                 [] { return std::make_unique<TerrainAutoMaterialOp>(); });
    registry.add({"terrain.paint_stroke", "Paint Terrain Layer", entt::resolve<TerrainPaintStrokeOpParams>()},
                 [] { return std::make_unique<TerrainPaintStrokeOp>(); });
}

} // namespace f3d::terrain
