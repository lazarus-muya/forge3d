// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/entity_ops.hpp"

#include "f3d/ops/prefab.hpp"

#include "f3d/core/reflect.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace f3d::ops {
namespace {

using core::Guid;
using scene::World;

// --- Parameters ------------------------------------------------------------------------------

struct CreateParams {
    std::string name;
    Guid parent; ///< null: a root
    Guid guid;   ///< optional explicit GUID (redo/tests); null: generate
};

struct DeleteParams {
    Guid entity;
};

struct SetPropertyParams {
    Guid entity;
    std::string path; ///< "<Component>.<field>", e.g. "Transform.translation"
};

struct ReparentParams {
    Guid entity;
    Guid parent;             ///< null: a root
    std::int32_t index = -1; ///< -1: append
};

struct DuplicateParams {
    Guid entity;
    Guid guid; ///< optional GUID for the copy's root (so the caller can select it); null: generate
};

void register_param_types() {
    entt::meta_factory<CreateParams>{}
        .type("entity.create.params")
        .data<&CreateParams::name>("name")
        .data<&CreateParams::parent>("parent")
        .data<&CreateParams::guid>("guid");
    entt::meta_factory<DeleteParams>{}.type("entity.delete.params").data<&DeleteParams::entity>("entity");
    entt::meta_factory<SetPropertyParams>{}
        .type("entity.set_property.params")
        .data<&SetPropertyParams::entity>("entity")
        .data<&SetPropertyParams::path>("path");
    entt::meta_factory<ReparentParams>{}
        .type("entity.reparent.params")
        .data<&ReparentParams::entity>("entity")
        .data<&ReparentParams::parent>("parent")
        .data<&ReparentParams::index>("index");
    entt::meta_factory<DuplicateParams>{}
        .type("entity.duplicate.params")
        .data<&DuplicateParams::entity>("entity")
        .data<&DuplicateParams::guid>("guid");
}

[[nodiscard]] entt::entity resolve(const World& world, const Guid& guid) {
    return guid.is_null() ? entt::null : world.find(guid);
}

// --- entity.create ---------------------------------------------------------------------------

class EntityCreateOp final : public OperationBase<CreateParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "entity.create"; }
    [[nodiscard]] std::string display_name() const override { return "Create " + params.name; }

    bool execute(Context& context) override {
        const entt::entity parent = resolve(context.world, params.parent);
        if (!params.parent.is_null() && parent == entt::null) {
            return false;
        }
        if (!params.guid.is_null()) {
            created_ = params.guid;
        } else if (created_.is_null()) {
            created_ = Guid::generate();
        }
        if (context.world.create(params.name, parent, created_) == entt::null) {
            return false;
        }
        context.changes.emit({ChangeKind::EntityCreated, created_});
        return true;
    }

    void undo(Context& context) override {
        const entt::entity e = context.world.find(created_);
        if (e != entt::null) {
            context.world.destroy(e);
            context.changes.emit({ChangeKind::EntityDestroyed, created_});
        }
    }

private:
    Guid created_;
};

// --- entity.delete ---------------------------------------------------------------------------

class EntityDeleteOp final : public OperationBase<DeleteParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "entity.delete"; }
    [[nodiscard]] std::string display_name() const override { return "Delete entity"; }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null) {
            return false;
        }
        snapshot_ = scene::snapshot_subtree(context.world, e);
        snapshot_bytes_ = snapshot_.dump().size();
        const entt::entity parent = context.world.parent(e);
        parent_guid_ = parent == entt::null ? Guid{} : context.world.guid(parent);
        index_ = context.world.sibling_index(e);
        context.world.destroy(e);
        context.changes.emit({ChangeKind::EntityDestroyed, params.entity});
        return true;
    }

    void undo(Context& context) override {
        const entt::entity parent = resolve(context.world, parent_guid_);
        scene::restore_subtree(context.world, snapshot_, parent, index_, false);
        context.changes.emit({ChangeKind::EntityCreated, params.entity});
    }

    [[nodiscard]] std::size_t memory_size() const override { return sizeof(*this) + snapshot_bytes_; }

private:
    nlohmann::json snapshot_;
    std::size_t snapshot_bytes_ = 0;
    Guid parent_guid_;
    std::size_t index_ = 0;
};

// --- entity.set_property ---------------------------------------------------------------------

class EntitySetPropertyOp final : public OperationBase<SetPropertyParams> {
public:
    nlohmann::json value;

    [[nodiscard]] std::string_view id() const override { return "entity.set_property"; }
    [[nodiscard]] std::string display_name() const override { return "Set " + params.path; }

    [[nodiscard]] nlohmann::json params_json() const override {
        nlohmann::json json = OperationBase::params_json();
        json["value"] = value;
        return json;
    }

    void set_params_json(const nlohmann::json& json) override {
        OperationBase::set_params_json(json);
        value = json.contains("value") ? json.at("value") : nlohmann::json();
    }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null) {
            return false;
        }
        entt::meta_any component;
        entt::meta_data field;
        if (!locate(context, e, component, field)) {
            return false;
        }
        if (!has_old_) {
            old_value_ = nlohmann::json();
            core::reflect::value_to_json(old_value_, field.get(component));
            has_old_ = true;
        }
        entt::meta_any new_value;
        try {
            new_value = core::reflect::value_from_json(value, field.type());
        } catch (const std::exception&) {
            return false; // malformed value for this field
        }
        if (!field.set(component, std::move(new_value))) {
            return false;
        }
        // An edit of a prefab instance's entity is an override of that instance.
        if (!has_override_undo_) {
            override_undo_ = record_override(context.world, e, params.path, value);
            has_override_undo_ = true;
        } else {
            record_override(context.world, e, params.path, value);
        }
        changed(context, e);
        return true;
    }

    void undo(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null || !has_old_) {
            return;
        }
        entt::meta_any component;
        entt::meta_data field;
        if (!locate(context, e, component, field)) {
            return;
        }
        field.set(component, core::reflect::value_from_json(old_value_, field.type()));
        restore_overrides(context.world, override_undo_);
        has_override_undo_ = false;
        changed(context, e);
    }

    bool merge(const Operation& next) override {
        const auto* other = dynamic_cast<const EntitySetPropertyOp*>(&next);
        if (other == nullptr || other->params.entity != params.entity || other->params.path != params.path) {
            return false;
        }
        value = other->value; // keep old_value_: undo returns to the pre-drag state
        return true;
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + params.path.size() + 2 * sizeof(nlohmann::json) * (1 + value.size());
    }

private:
    /// Fields were written through reflection, behind World's back: keep its caches in step.
    void changed(Context& context, entt::entity e) const {
        if (params.path.starts_with("Transform.")) {
            context.world.mark_dirty(e);
        }
        context.changes.emit({ChangeKind::ComponentChanged, params.entity});
    }

    [[nodiscard]] bool locate(Context& context, entt::entity e, entt::meta_any& component,
                              entt::meta_data& field) const {
        const auto dot = params.path.find('.');
        if (dot == std::string::npos) {
            return false;
        }
        const std::string component_name = params.path.substr(0, dot);
        const std::string field_name = params.path.substr(dot + 1);
        component = scene::component_by_name(context.world.registry(), e, component_name);
        if (!component) {
            return false;
        }
        field = core::reflect::field_by_name(component.type(), field_name);
        return static_cast<bool>(field);
    }

    nlohmann::json old_value_;
    bool has_old_ = false;
    OverrideUndo override_undo_;
    bool has_override_undo_ = false;
};

// --- entity.reparent -------------------------------------------------------------------------

class EntityReparentOp final : public OperationBase<ReparentParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "entity.reparent"; }
    [[nodiscard]] std::string display_name() const override { return "Reparent entity"; }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null) {
            return false;
        }
        const entt::entity parent = resolve(context.world, params.parent);
        if (!params.parent.is_null() && parent == entt::null) {
            return false;
        }
        if (!has_old_) {
            const entt::entity old_parent = context.world.parent(e);
            old_parent_guid_ = old_parent == entt::null ? Guid{} : context.world.guid(old_parent);
            old_index_ = context.world.sibling_index(e);
            const scene::Transform& transform = context.world.transform(e);
            old_translation_ = transform.translation;
            old_rotation_ = transform.rotation;
            old_scale_ = transform.scale;
            has_old_ = true;
        }
        const std::size_t index = params.index < 0 ? World::kAppend : static_cast<std::size_t>(params.index);
        if (!context.world.set_parent(e, parent, index, true)) {
            return false;
        }
        context.changes.emit({ChangeKind::HierarchyChanged, params.entity});
        return true;
    }

    void undo(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null || !has_old_) {
            return;
        }
        const entt::entity parent = resolve(context.world, old_parent_guid_);
        context.world.set_parent(e, parent, old_index_, false);
        context.world.set_local(e, old_translation_, old_rotation_, old_scale_);
        context.changes.emit({ChangeKind::HierarchyChanged, params.entity});
    }

private:
    Guid old_parent_guid_;
    std::size_t old_index_ = 0;
    glm::vec3 old_translation_{0.0f};
    glm::quat old_rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 old_scale_{1.0f};
    bool has_old_ = false;
};

// --- entity.duplicate ------------------------------------------------------------------------

class EntityDuplicateOp final : public OperationBase<DuplicateParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "entity.duplicate"; }
    [[nodiscard]] std::string display_name() const override { return "Duplicate entity"; }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null) {
            return false;
        }
        if (created_guid_.is_null()) {
            // First run: snapshot the source, then recreate it with fresh GUIDs.
            const nlohmann::json source = scene::snapshot_subtree(context.world, e);
            const entt::entity parent = context.world.parent(e);
            parent_guid_ = parent == entt::null ? Guid{} : context.world.guid(parent);
            index_ = context.world.sibling_index(e) + 1;
            const entt::entity parent_entity = resolve(context.world, parent_guid_);
            if (!params.guid.is_null() && context.world.find(params.guid) != entt::null) {
                return false; // the requested GUID is taken
            }
            const entt::entity copy =
                scene::restore_subtree(context.world, source, parent_entity, index_, true, params.guid);
            if (copy == entt::null) {
                return false;
            }
            created_guid_ = context.world.guid(copy);
            created_snapshot_ = scene::snapshot_subtree(context.world, copy);
            snapshot_bytes_ = created_snapshot_.dump().size();
        } else {
            const entt::entity parent_entity = resolve(context.world, parent_guid_);
            if (scene::restore_subtree(context.world, created_snapshot_, parent_entity, index_, false) == entt::null) {
                return false;
            }
        }
        context.changes.emit({ChangeKind::EntityCreated, created_guid_});
        return true;
    }

    void undo(Context& context) override {
        const entt::entity e = context.world.find(created_guid_);
        if (e != entt::null) {
            context.world.destroy(e);
            context.changes.emit({ChangeKind::EntityDestroyed, created_guid_});
        }
    }

    [[nodiscard]] std::size_t memory_size() const override { return sizeof(*this) + snapshot_bytes_; }

private:
    Guid created_guid_;
    Guid parent_guid_;
    std::size_t index_ = 0;
    nlohmann::json created_snapshot_;
    std::size_t snapshot_bytes_ = 0;
};

} // namespace

void register_core_ops(OpRegistry& registry) {
    register_param_types();
    registry.add({"entity.create", "Create Entity", entt::resolve<CreateParams>()},
                 [] { return std::make_unique<EntityCreateOp>(); });
    registry.add({"entity.delete", "Delete Entity", entt::resolve<DeleteParams>()},
                 [] { return std::make_unique<EntityDeleteOp>(); });
    registry.add({"entity.set_property", "Set Property", entt::resolve<SetPropertyParams>(),
                  nlohmann::json::array(
                      {{{"name", "value"}, {"type", "json"}, {"tooltip", "The new value, in the field's JSON form"}}})},
                 [] { return std::make_unique<EntitySetPropertyOp>(); });
    registry.add({"entity.reparent", "Reparent Entity", entt::resolve<ReparentParams>()},
                 [] { return std::make_unique<EntityReparentOp>(); });
    registry.add({"entity.duplicate", "Duplicate Entity", entt::resolve<DuplicateParams>()},
                 [] { return std::make_unique<EntityDuplicateOp>(); });
    register_component_ops(registry);
}

} // namespace f3d::ops
