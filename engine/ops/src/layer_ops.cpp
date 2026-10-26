// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/builtin.hpp"

#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace f3d::ops {
namespace {

using core::Guid;
using scene::LayerInfo;
using scene::LayerTable;

struct LayerCreateParams {
    std::string name;
    Guid guid;               ///< optional explicit id; null: generate
    std::int32_t index = -1; ///< position in the table; -1: last
};

struct LayerRefParams {
    Guid layer;
};

struct LayerRenameParams {
    Guid layer;
    std::string name;
};

struct LayerFlagParams {
    Guid layer;
    bool value = true;
};

struct LayerAssignParams {
    std::vector<Guid> entities;
    Guid layer; ///< null: the default layer
};

void register_param_types() {
    core::reflect::register_value_types();
    entt::meta_factory<LayerCreateParams>{}
        .type("layer.create.params")
        .data<&LayerCreateParams::name>("name")
        .data<&LayerCreateParams::guid>("guid")
        .data<&LayerCreateParams::index>("index");
    entt::meta_factory<LayerRefParams>{}.type("layer.ref.params").data<&LayerRefParams::layer>("layer");
    entt::meta_factory<LayerRenameParams>{}
        .type("layer.rename.params")
        .data<&LayerRenameParams::layer>("layer")
        .data<&LayerRenameParams::name>("name");
    entt::meta_factory<LayerFlagParams>{}
        .type("layer.flag.params")
        .data<&LayerFlagParams::layer>("layer")
        .data<&LayerFlagParams::value>("value");
    entt::meta_factory<LayerAssignParams>{}
        .type("layer.assign.params")
        .data<&LayerAssignParams::entities>("entities")
        .data<&LayerAssignParams::layer>("layer");
}

void layers_changed(Context& context) {
    context.changes.emit({ChangeKind::LayersChanged, {}});
}

/// Puts `e` on `layer` (null: removes the Layer component, i.e. the default layer).
void set_entity_layer(scene::World& world, entt::entity e, const Guid& layer) {
    if (layer.is_null()) {
        world.registry().remove<scene::Layer>(e);
    } else {
        world.registry().emplace_or_replace<scene::Layer>(e, scene::Layer{layer});
    }
}

// --- layer.create ----------------------------------------------------------------------------

class LayerCreateOp final : public OperationBase<LayerCreateParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "layer.create"; }
    [[nodiscard]] std::string display_name() const override { return "Create layer " + params.name; }

    bool execute(Context& context) override {
        if (created_.is_null()) {
            created_ = params.guid.is_null() ? Guid::generate() : params.guid;
        }
        const std::size_t index = params.index < 0 ? LayerTable::npos : static_cast<std::size_t>(params.index);
        if (!context.world.layers().insert({created_, params.name, true, false}, index)) {
            return false;
        }
        layers_changed(context);
        return true;
    }

    void undo(Context& context) override {
        context.world.layers().remove(created_);
        layers_changed(context);
    }

private:
    Guid created_;
};

// --- layer.delete ----------------------------------------------------------------------------

class LayerDeleteOp final : public OperationBase<LayerRefParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "layer.delete"; }
    [[nodiscard]] std::string display_name() const override { return "Delete layer " + deleted_.name; }

    bool execute(Context& context) override {
        LayerTable& layers = context.world.layers();
        const LayerInfo* info = layers.find(params.layer);
        if (!info) {
            return false;
        }
        deleted_ = *info;
        index_ = layers.index_of(params.layer);
        // Its entities fall back to the default layer.
        members_.clear();
        auto& registry = context.world.registry();
        for (const auto [e, layer] : registry.view<scene::Layer>().each()) {
            if (layer.layer == params.layer) {
                members_.push_back(context.world.guid(e));
            }
        }
        for (const Guid& guid : members_) {
            const entt::entity e = context.world.find(guid);
            set_entity_layer(context.world, e, {});
            context.changes.emit({ChangeKind::ComponentChanged, guid});
        }
        layers.remove(params.layer);
        layers_changed(context);
        return true;
    }

    void undo(Context& context) override {
        context.world.layers().insert(deleted_, index_);
        for (const Guid& guid : members_) {
            if (const entt::entity e = context.world.find(guid); e != entt::null) {
                set_entity_layer(context.world, e, deleted_.id);
                context.changes.emit({ChangeKind::ComponentChanged, guid});
            }
        }
        layers_changed(context);
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + deleted_.name.size() + members_.size() * sizeof(Guid);
    }

private:
    LayerInfo deleted_;
    std::size_t index_ = 0;
    std::vector<Guid> members_;
};

// --- layer.rename / set_visible / set_locked -------------------------------------------------

class LayerRenameOp final : public OperationBase<LayerRenameParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "layer.rename"; }
    [[nodiscard]] std::string display_name() const override { return "Rename layer to " + params.name; }

    bool execute(Context& context) override {
        LayerInfo* info = context.world.layers().find(params.layer);
        if (!info) {
            return false;
        }
        old_name_ = std::exchange(info->name, params.name);
        layers_changed(context);
        return true;
    }

    void undo(Context& context) override {
        if (LayerInfo* info = context.world.layers().find(params.layer)) {
            info->name = old_name_;
        }
        layers_changed(context);
    }

private:
    std::string old_name_;
};

/// layer.set_visible and layer.set_locked: flip one flag of a layer row.
class LayerFlagOp final : public OperationBase<LayerFlagParams> {
public:
    LayerFlagOp(const char* id, bool LayerInfo::* flag) : id_(id), flag_(flag) {}

    [[nodiscard]] std::string_view id() const override { return id_; }
    [[nodiscard]] std::string display_name() const override {
        const bool visible = flag_ == &LayerInfo::visible;
        return visible ? (params.value ? "Show layer" : "Hide layer") : (params.value ? "Lock layer" : "Unlock layer");
    }

    bool execute(Context& context) override {
        LayerInfo* info = context.world.layers().find(params.layer);
        if (!info) {
            return false;
        }
        old_value_ = std::exchange(info->*flag_, params.value);
        layers_changed(context);
        return true;
    }

    void undo(Context& context) override {
        if (LayerInfo* info = context.world.layers().find(params.layer)) {
            info->*flag_ = old_value_;
        }
        layers_changed(context);
    }

private:
    const char* id_;
    bool LayerInfo::* flag_;
    bool old_value_ = false;
};

// --- layer.assign ----------------------------------------------------------------------------

class LayerAssignOp final : public OperationBase<LayerAssignParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "layer.assign"; }
    [[nodiscard]] std::string display_name() const override { return "Assign to layer"; }

    bool execute(Context& context) override {
        if (!params.layer.is_null() && !context.world.layers().find(params.layer)) {
            return false;
        }
        previous_.clear();
        for (const Guid& guid : params.entities) {
            const entt::entity e = context.world.find(guid);
            if (e == entt::null) {
                continue; // unknown entities are skipped
            }
            previous_.emplace_back(guid, scene::layer_of(context.world, e));
            set_entity_layer(context.world, e, params.layer);
            context.changes.emit({ChangeKind::ComponentChanged, guid});
        }
        return !previous_.empty();
    }

    void undo(Context& context) override {
        for (const auto& [guid, layer] : previous_) {
            if (const entt::entity e = context.world.find(guid); e != entt::null) {
                set_entity_layer(context.world, e, layer);
                context.changes.emit({ChangeKind::ComponentChanged, guid});
            }
        }
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (params.entities.size() + 2 * previous_.size()) * sizeof(Guid);
    }

private:
    std::vector<std::pair<Guid, Guid>> previous_; ///< entity, its layer before
};

} // namespace

void register_layer_ops(OpRegistry& registry) {
    register_param_types();
    registry.add({"layer.create", "Create Layer", entt::resolve<LayerCreateParams>()},
                 [] { return std::make_unique<LayerCreateOp>(); });
    registry.add({"layer.delete", "Delete Layer", entt::resolve<LayerRefParams>()},
                 [] { return std::make_unique<LayerDeleteOp>(); });
    registry.add({"layer.rename", "Rename Layer", entt::resolve<LayerRenameParams>()},
                 [] { return std::make_unique<LayerRenameOp>(); });
    registry.add({"layer.set_visible", "Set Layer Visibility", entt::resolve<LayerFlagParams>()},
                 [] { return std::make_unique<LayerFlagOp>("layer.set_visible", &LayerInfo::visible); });
    registry.add({"layer.set_locked", "Set Layer Lock", entt::resolve<LayerFlagParams>()},
                 [] { return std::make_unique<LayerFlagOp>("layer.set_locked", &LayerInfo::locked); });
    registry.add({"layer.assign", "Assign to Layer", entt::resolve<LayerAssignParams>()},
                 [] { return std::make_unique<LayerAssignOp>(); });
}

} // namespace f3d::ops
