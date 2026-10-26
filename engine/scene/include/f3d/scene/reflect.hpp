// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>
#include <entt/meta/meta.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::scene {

/// Registers the scene's component and enum types with the reflection registry
/// (f3d/core/reflect.hpp). Idempotent; called once at start-up and by tests.
void register_types();

/// Calls `fn` with a meta_any referencing each reflected component present on
/// `e`, in a fixed order (Name, Transform, MeshRenderer, Light, Camera, Layer,
/// PrefabInstance, PrefabLink). The
/// meta_any aliases the live component, so edits through it change the entity.
void for_each_component(entt::registry& registry, entt::entity e, const std::function<void(entt::meta_any)>& fn);

/// A meta_any referencing the component of the given reflected type name on `e`,
/// or an invalid meta_any. `type_name` is e.g. "MeshRenderer".
[[nodiscard]] entt::meta_any component_by_name(entt::registry& registry, entt::entity e, std::string_view type_name);

/// Default-constructs the component of the given reflected type name on `e`.
/// Returns false for an unknown type name.
bool add_component(entt::registry& registry, entt::entity e, std::string_view type_name);

/// Removes the optional component of the given type name. Returns false if it
/// isn't an optional component type or `e` doesn't have it.
bool remove_component(entt::registry& registry, entt::entity e, std::string_view type_name);

/// Optional components a user can add/remove (entity.add_component): MeshRenderer,
/// Light, Camera, Sky, ReflectionProbe, and those other modules register as addable.
/// (Name/Transform are always present; Layer belongs to layer.*.)
[[nodiscard]] std::vector<std::string_view> addable_components();
[[nodiscard]] bool is_addable_component(std::string_view type_name);

/// An optional component type known to reflection, snapshots, .forge files and the
/// entity.add/remove_component operations.
struct ComponentType {
    std::string name;
    bool addable = true; ///< offered by entity.add_component and the inspector
    std::function<bool(const entt::registry&, entt::entity)> has;
    std::function<entt::meta_any(entt::registry&, entt::entity)> get; ///< aliases the live component
    std::function<void(entt::registry&, entt::entity)> add;           ///< default-constructs (or replaces)
    std::function<bool(entt::registry&, entt::entity)> remove;
};

/// Registers an optional component type (modules above scene: terrain, world, ...).
/// Its fields must be registered with entt::meta under the same name. Re-registering
/// a name replaces it.
void register_component_type(ComponentType type);

template <class T>
void register_component(const char* name, bool addable = true) {
    register_component_type(ComponentType{
        name,
        addable,
        [](const entt::registry& r, entt::entity e) { return r.all_of<T>(e); },
        [](entt::registry& r, entt::entity e) {
            auto* c = r.try_get<T>(e);
            return c ? entt::forward_as_meta(*c) : entt::meta_any{};
        },
        [](entt::registry& r, entt::entity e) { r.emplace_or_replace<T>(e); },
        [](entt::registry& r, entt::entity e) { return r.remove<T>(e) > 0; },
    });
}

/// Every registered optional component type, in registration order.
[[nodiscard]] const std::vector<ComponentType>& component_types();

} // namespace f3d::scene
