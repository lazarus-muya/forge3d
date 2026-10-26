// SPDX-License-Identifier: Apache-2.0
#include "f3d/world/world_ops.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/ops/operation.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/scatter_paint.hpp"
#include "f3d/world/spline.hpp"
#include "f3d/world/water.hpp"
#include "f3d/terrain/builtin_textures.hpp"
#include "f3d/terrain/terrain.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace f3d::world {
namespace {

// Globally unique names: EnTT identifies types by name (see prefab_ops.cpp).
struct ScatterPaintStrokeOpParams {
    core::Guid scatter;
    bool erase = false;
    float radius = 8.0f;
    float strength = 0.5f;
    std::string falloff = "smooth";
    float spacing = 0.25f;
    std::vector<glm::vec3> points; ///< world x, z, pressure
};

struct SplineCreateOpParams {
    std::string name;
    core::Guid guid;
    std::string kind = "road";
    core::Guid terrain;
    std::vector<glm::vec3> points; ///< world
    std::string paint_surface = "auto";
};

struct WaterCreateOpParams {
    std::string name;
    core::Guid guid;
    std::string kind = "lake";
    core::Guid terrain;
    glm::vec3 position{0.0f}; ///< world; y is the water level
};

void register_param_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        entt::meta_factory<ScatterPaintStrokeOpParams>{}
            .type("scatter.paint_stroke.params")
            .data<&ScatterPaintStrokeOpParams::scatter>("scatter")
            .data<&ScatterPaintStrokeOpParams::erase>("erase")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Remove instances instead of adding them"))
            .data<&ScatterPaintStrokeOpParams::radius>("radius")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.1, 1.0e4, 0.1))
            .data<&ScatterPaintStrokeOpParams::strength>("strength")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1.0, 0.01))
            .data<&ScatterPaintStrokeOpParams::falloff>("falloff")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("smooth, linear, sphere or constant"))
            .data<&ScatterPaintStrokeOpParams::spacing>("spacing")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.01, 4.0, 0.01))
            .data<&ScatterPaintStrokeOpParams::points>("points")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("[x, z, pressure] per point, world metres"));
        entt::meta_factory<SplineCreateOpParams>{}
            .type("spline.create.params")
            .data<&SplineCreateOpParams::name>("name")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Empty: after the kind (Road, Path...)"))
            .data<&SplineCreateOpParams::guid>("guid")
            .data<&SplineCreateOpParams::kind>("kind")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("road, path, river, fence or custom"))
            .data<&SplineCreateOpParams::terrain>("terrain")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("The terrain it shapes and paints (may be null)"))
            .data<&SplineCreateOpParams::points>("points")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Control points, world metres; the entity sits at the first"))
            .data<&SplineCreateOpParams::paint_surface>("paint_surface")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip(
                "Built-in surface painted under it, added to the terrain's layers if missing; \"auto\": "
                "dirt for roads and paths, gravel for rivers; empty: none"));
        entt::meta_factory<WaterCreateOpParams>{}
            .type("water.create.params")
            .data<&WaterCreateOpParams::name>("name")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Empty: Lake or Ocean"))
            .data<&WaterCreateOpParams::guid>("guid")
            .data<&WaterCreateOpParams::kind>("kind")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("lake or ocean"))
            .data<&WaterCreateOpParams::terrain>("terrain")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("Lakes: the terrain whose basin fills (null: the one below)"))
            .data<&WaterCreateOpParams::position>("position")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip(
                "World position: lakes fill the basin under it; its height is the water level"));
        return true;
    }();
    static_cast<void>(registered);
}

class ScatterPaintStrokeOp final : public ops::OperationBase<ScatterPaintStrokeOpParams> {
public:
    ScatterPaintStrokeOp() = default;
    ScatterPaintStrokeOp(std::unique_ptr<ScatterPaintRecorder> applied, bool created)
        : applied_(std::move(applied)), created_(created) {}

    [[nodiscard]] std::string_view id() const override { return "scatter.paint_stroke"; }
    [[nodiscard]] std::string display_name() const override {
        return params.erase ? "Erase scatter" : "Paint scatter";
    }

    bool execute(ops::Context& context) override {
        scene::World& world = context.world;
        const entt::entity e = world.find(params.scatter);
        const Scatter* scatter = e == entt::null ? nullptr : world.registry().try_get<Scatter>(e);
        if (!scatter) {
            F3D_LOG_ERROR("scatter.paint_stroke: {} is not a scatter", params.scatter.to_string());
            return false;
        }
        ScatterMaskStore& store = scatter_masks(world);
        if (applied_) { // painted live by the editor
            diff_ = applied_->finish();
            applied_.reset();
            context.changes.emit({ops::ChangeKind::ComponentChanged, params.scatter, {}});
            return true;
        }
        std::shared_ptr<ScatterMask> mask = store.find(params.scatter);
        if (!mask) {
            mask = new_scatter_mask(*scatter);
            store.set(params.scatter, mask);
            created_ = true;
        }
        if (diff_) {
            diff_->apply_after(*mask); // redo
        } else {
            terrain::Brush brush;
            brush.radius = params.radius;
            brush.strength = params.strength;
            brush.spacing = params.spacing;
            if (!terrain::parse(params.falloff, brush.falloff) || !(params.radius > 0.0f) || params.points.empty()) {
                F3D_LOG_ERROR("scatter.paint_stroke: needs radius > 0, a known falloff and at least one point");
                if (created_) {
                    store.erase(params.scatter);
                }
                return false;
            }
            world.update_transforms();
            ScatterPaintRecorder stroke(mask, glm::vec3(world.transform(e).world[3]), scatter->area, brush,
                                        params.erase);
            for (const glm::vec3& p : params.points) {
                stroke.add({{p.x, p.y}, p.z});
            }
            diff_ = stroke.finish();
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.scatter, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        ScatterMaskStore& store = scatter_masks(context.world);
        if (created_) {
            store.erase(params.scatter); // back to no painting at all
        } else if (const auto mask = store.find(params.scatter); mask && diff_) {
            diff_->apply_before(*mask);
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, params.scatter, {}});
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (diff_ ? diff_->bytes() : 0) + params.points.size() * sizeof(glm::vec3);
    }

private:
    std::unique_ptr<ScatterPaintRecorder> applied_;
    std::optional<MaskDiff> diff_;
    bool created_ = false; ///< this stroke created the mask (undo removes it)
};

class SplineCreateOp final : public ops::OperationBase<SplineCreateOpParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "spline.create"; }
    [[nodiscard]] std::string display_name() const override { return "Create " + params.kind; }

    bool execute(ops::Context& context) override {
        scene::World& world = context.world;
        SplineKind kind = SplineKind::Road;
        if (!parse(params.kind, kind) || params.points.empty()) {
            F3D_LOG_ERROR("spline.create: needs a kind (road, path, river, fence, custom) and at least one point");
            return false;
        }
        const entt::entity t = params.terrain.is_null() ? entt::null : world.find(params.terrain);
        if (!params.terrain.is_null() && (t == entt::null || !world.registry().all_of<terrain::Terrain>(t))) {
            F3D_LOG_ERROR("spline.create: {} is not a terrain", params.terrain.to_string());
            return false;
        }
        if (created_.is_null()) {
            created_ = params.guid.is_null() ? core::Guid::generate() : params.guid;
        }
        std::string name = params.name;
        if (name.empty()) {
            name = std::string(to_string(kind));
            name[0] = static_cast<char>(name[0] - 'a' + 'A');
        }
        const entt::entity e = world.create(name, entt::null, created_);
        if (e == entt::null) {
            return false;
        }
        Spline spline = spline_preset(kind);
        spline.terrain = params.terrain;
        const glm::vec3 origin = params.points.front();
        for (const glm::vec3& p : params.points) {
            spline.points.push_back(p - origin);
        }
        // The layer painted under it: an existing one with the surface's name, or a new one.
        std::string surface = params.paint_surface;
        if (surface == "auto") {
            // Paths are dirt; roads get a dirt shoulder under the paving; riverbeds are gravel.
            surface = kind == SplineKind::Path || kind == SplineKind::Road ? "dirt"
                      : kind == SplineKind::River                        ? "gravel"
                                                                         : "";
        }
        if (!surface.empty() && t != entt::null) {
            terrain::Terrain& ground = world.registry().get<terrain::Terrain>(t);
            const terrain::TerrainLayer layer = terrain::builtin_layer(surface);
            const auto it = std::find_if(ground.layers.begin(), ground.layers.end(),
                                         [&](const terrain::TerrainLayer& l) { return l.name == layer.name; });
            if (it != ground.layers.end()) {
                spline.paint_layer = static_cast<std::int32_t>(it - ground.layers.begin());
            } else if (ground.layers.size() < terrain::kMaxLayers && !layer.albedo_texture.empty()) {
                layers_before_ = ground.layers;
                added_layer_ = true;
                if (ground.layers.empty()) {
                    ground.layers.emplace_back(); // keep the base what it was
                }
                ground.layers.push_back(layer);
                spline.paint_layer = static_cast<std::int32_t>(ground.layers.size() - 1);
                context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
            }
        }
        world.set_local(e, origin, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
        world.registry().emplace<Spline>(e, std::move(spline));
        context.changes.emit({ops::ChangeKind::EntityCreated, created_, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        scene::World& world = context.world;
        if (const entt::entity e = world.find(created_); e != entt::null) {
            world.destroy(e);
            context.changes.emit({ops::ChangeKind::EntityDestroyed, created_, {}});
        }
        if (added_layer_) {
            if (const entt::entity t = world.find(params.terrain);
                t != entt::null && world.registry().all_of<terrain::Terrain>(t)) {
                world.registry().get<terrain::Terrain>(t).layers = layers_before_;
                context.changes.emit({ops::ChangeKind::ComponentChanged, params.terrain, {}});
            }
            added_layer_ = false;
        }
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + params.points.size() * sizeof(glm::vec3) +
               layers_before_.size() * sizeof(terrain::TerrainLayer);
    }

private:
    core::Guid created_;
    bool added_layer_ = false;
    std::vector<terrain::TerrainLayer> layers_before_;
};

class WaterCreateOp final : public ops::OperationBase<WaterCreateOpParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "water.create"; }
    [[nodiscard]] std::string display_name() const override { return "Create " + params.kind; }

    bool execute(ops::Context& context) override {
        scene::World& world = context.world;
        WaterKind kind = WaterKind::Lake;
        if (!parse(params.kind, kind)) {
            F3D_LOG_ERROR("water.create: kind must be lake or ocean");
            return false;
        }
        if (!params.terrain.is_null()) {
            const entt::entity t = world.find(params.terrain);
            if (t == entt::null || !world.registry().all_of<terrain::Terrain>(t)) {
                F3D_LOG_ERROR("water.create: {} is not a terrain", params.terrain.to_string());
                return false;
            }
        }
        if (created_.is_null()) {
            created_ = params.guid.is_null() ? core::Guid::generate() : params.guid;
        }
        const std::string name = !params.name.empty() ? params.name : kind == WaterKind::Lake ? "Lake" : "Ocean";
        const entt::entity e = world.create(name, entt::null, created_);
        if (e == entt::null) {
            return false;
        }
        WaterBody water = water_preset(kind);
        water.terrain = params.terrain;
        world.set_local(e, params.position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
        world.registry().emplace<WaterBody>(e, water);
        context.changes.emit({ops::ChangeKind::EntityCreated, created_, {}});
        return true;
    }

    void undo(ops::Context& context) override {
        if (const entt::entity e = context.world.find(created_); e != entt::null) {
            context.world.destroy(e);
            context.changes.emit({ops::ChangeKind::EntityDestroyed, created_, {}});
        }
    }

private:
    core::Guid created_;
};

} // namespace

void register_world_ops(ops::OpRegistry& registry) {
    register_types();
    register_spline_types();
    register_water_types();
    register_param_types();
    registry.add({"scatter.paint_stroke", "Paint Scatter", entt::resolve<ScatterPaintStrokeOpParams>()},
                 [] { return std::make_unique<ScatterPaintStrokeOp>(); });
    registry.add({"spline.create", "Create Spline", entt::resolve<SplineCreateOpParams>()},
                 [] { return std::make_unique<SplineCreateOp>(); });
    registry.add({"water.create", "Create Water", entt::resolve<WaterCreateOpParams>()},
                 [] { return std::make_unique<WaterCreateOp>(); });
}

std::unique_ptr<ops::Operation> make_scatter_paint_op(const core::Guid& scatter,
                                                      std::unique_ptr<ScatterPaintRecorder> applied,
                                                      bool created_mask) {
    ScatterPaintStrokeOpParams p;
    p.scatter = scatter;
    p.erase = applied->erase();
    p.radius = applied->brush().radius;
    p.strength = applied->brush().strength;
    p.falloff = std::string(terrain::to_string(applied->brush().falloff));
    p.spacing = applied->brush().spacing;
    for (const terrain::StrokePoint& point : applied->points()) {
        p.points.emplace_back(point.position.x, point.position.y, point.pressure);
    }
    auto op = std::make_unique<ScatterPaintStrokeOp>(std::move(applied), created_mask);
    op->params = std::move(p);
    return op;
}

} // namespace f3d::world
