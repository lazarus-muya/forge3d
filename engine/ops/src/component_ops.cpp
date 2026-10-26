// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/reflect_json.hpp"
#include "f3d/ops/entity_ops.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <exception>
#include <memory>
#include <string>

namespace f3d::ops {
namespace {

using core::Guid;

struct ComponentParams {
    Guid entity;
    std::string component; ///< reflected type name: "MeshRenderer", "Light", "Camera"
};

/// entity.add_component: adds a component (default values, then `value` applied).
class AddComponentOp final : public OperationBase<ComponentParams> {
public:
    nlohmann::json value; ///< optional initial field values (the component's JSON form)

    [[nodiscard]] std::string_view id() const override { return "entity.add_component"; }
    [[nodiscard]] std::string display_name() const override { return "Add " + params.component; }

    [[nodiscard]] nlohmann::json params_json() const override {
        nlohmann::json json = OperationBase::params_json();
        if (!value.is_null()) {
            json["value"] = value;
        }
        return json;
    }
    void set_params_json(const nlohmann::json& json) override {
        OperationBase::set_params_json(json);
        value = json.contains("value") ? json.at("value") : nlohmann::json();
    }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null || !scene::is_addable_component(params.component) ||
            scene::component_by_name(context.world.registry(), e, params.component)) {
            return false; // unknown entity or type, or already present
        }
        if (!scene::add_component(context.world.registry(), e, params.component)) {
            return false;
        }
        if (value.is_object()) {
            try {
                entt::meta_any component = scene::component_by_name(context.world.registry(), e, params.component);
                core::reflect::apply_json(component, value);
            } catch (const std::exception&) {
                scene::remove_component(context.world.registry(), e, params.component);
                return false;
            }
        }
        context.changes.emit({ChangeKind::ComponentChanged, params.entity});
        return true;
    }

    void undo(Context& context) override {
        if (const entt::entity e = context.world.find(params.entity); e != entt::null) {
            scene::remove_component(context.world.registry(), e, params.component);
            context.changes.emit({ChangeKind::ComponentChanged, params.entity});
        }
    }
};

/// entity.remove_component: removes a component; undo restores its values.
class RemoveComponentOp final : public OperationBase<ComponentParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "entity.remove_component"; }
    [[nodiscard]] std::string display_name() const override { return "Remove " + params.component; }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null || !scene::is_addable_component(params.component)) {
            return false;
        }
        const entt::meta_any component = scene::component_by_name(context.world.registry(), e, params.component);
        if (!component) {
            return false;
        }
        saved_ = nlohmann::json();
        core::reflect::value_to_json(saved_, component);
        scene::remove_component(context.world.registry(), e, params.component);
        context.changes.emit({ChangeKind::ComponentChanged, params.entity});
        return true;
    }

    void undo(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (e == entt::null || !scene::add_component(context.world.registry(), e, params.component)) {
            return;
        }
        entt::meta_any component = scene::component_by_name(context.world.registry(), e, params.component);
        core::reflect::apply_json(component, saved_);
        context.changes.emit({ChangeKind::ComponentChanged, params.entity});
    }

    [[nodiscard]] std::size_t memory_size() const override { return sizeof(*this) + saved_.dump().size(); }

private:
    nlohmann::json saved_;
};

} // namespace

void register_component_ops(OpRegistry& registry) {
    entt::meta_factory<ComponentParams>{}
        .type("entity.component.params")
        .data<&ComponentParams::entity>("entity")
        .data<&ComponentParams::component>("component");
    registry.add({"entity.add_component", "Add Component", entt::resolve<ComponentParams>(),
                  nlohmann::json::array({{{"name", "value"},
                                          {"type", "json"},
                                          {"tooltip", "Optional initial field values (the component's JSON form)"}}})},
                 [] { return std::make_unique<AddComponentOp>(); });
    registry.add({"entity.remove_component", "Remove Component", entt::resolve<ComponentParams>()},
                 [] { return std::make_unique<RemoveComponentOp>(); });
}

} // namespace f3d::ops
