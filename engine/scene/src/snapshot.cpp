// SPDX-License-Identifier: Apache-2.0
#include "f3d/scene/snapshot.hpp"

#include "f3d/core/reflect.hpp"
#include "f3d/core/reflect_json.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>

namespace f3d::scene {
namespace {

void write_components(const World& world, entt::entity e, nlohmann::json& out) {
    const entt::registry& registry = world.registry();
    const auto write = [&]<class T>(const char* name) {
        if (const T* component = registry.try_get<T>(e)) {
            out[name] = core::reflect::to_json(*component);
        }
    };
    out = nlohmann::json::object();
    write.template operator()<Name>("Name");
    write.template operator()<Transform>("Transform");
    // Optional components: every registered type (other modules add theirs).
    entt::registry& mutable_registry = const_cast<entt::registry&>(registry);
    for (const ComponentType& type : component_types()) {
        if (type.has(registry, e)) {
            core::reflect::value_to_json(out[type.name], type.get(mutable_registry, e));
        }
    }
}

} // namespace

nlohmann::json snapshot_subtree(const World& world, entt::entity root) {
    nlohmann::json node;
    node["guid"] = world.guid(root).to_string();
    write_components(world, root, node["components"]);
    node["children"] = nlohmann::json::array();
    for (const entt::entity child : world.children(root)) {
        node["children"].push_back(snapshot_subtree(world, child));
    }
    return node;
}

nlohmann::json snapshot_world(const World& world) {
    nlohmann::json out;
    out["roots"] = nlohmann::json::array();
    for (const entt::entity root : world.roots()) {
        out["roots"].push_back(snapshot_subtree(world, root));
    }
    out["layers"] = nlohmann::json::array();
    for (const LayerInfo& layer : world.layers().layers()) {
        out["layers"].push_back(core::reflect::to_json(layer));
    }
    out["bookmarks"] = nlohmann::json::array();
    for (const CameraBookmark& bookmark : world.bookmarks().bookmarks()) {
        out["bookmarks"].push_back(core::reflect::to_json(bookmark));
    }
    return out;
}

bool restore_world(World& world, const nlohmann::json& snapshot, std::string* error) {
    world.clear();
    const auto fail = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    };
    try {
        if (!snapshot.is_object()) {
            return fail("the scene is not a JSON object");
        }
        if (snapshot.contains("layers")) {
            for (const auto& layer : snapshot.at("layers")) {
                if (!world.layers().insert(core::reflect::from_json<LayerInfo>(layer))) {
                    return fail("duplicate or null layer id");
                }
            }
        }
        if (snapshot.contains("bookmarks")) {
            for (const auto& bookmark : snapshot.at("bookmarks")) {
                world.bookmarks().set(core::reflect::from_json<CameraBookmark>(bookmark));
            }
        }
        if (snapshot.contains("roots")) {
            std::size_t index = 0;
            for (const auto& root : snapshot.at("roots")) {
                if (restore_subtree(world, root, entt::null, index++, false) == entt::null) {
                    return fail("entity " + root.value("guid", std::string("?")) + " could not be created");
                }
            }
        }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    world.update_transforms();
    return true;
}

entt::entity restore_subtree(World& world, const nlohmann::json& node, entt::entity parent, std::size_t index,
                             bool fresh_guids, const core::Guid& root_guid) {
    core::Guid guid = root_guid.is_null() ? core::Guid::generate() : root_guid;
    if (!fresh_guids) {
        guid = core::Guid::parse(node.at("guid").get<std::string>()).value_or(core::Guid::generate());
    }
    std::string name = "Entity";
    const nlohmann::json& components = node.contains("components") ? node.at("components") : nlohmann::json::object();
    if (components.contains("Name")) {
        name = components.at("Name").value("value", std::string("Entity"));
    }
    const entt::entity e = world.create(name, parent, guid, index);
    if (e == entt::null) {
        return entt::null;
    }
    if (components.contains("Transform")) {
        const Transform transform = core::reflect::from_json<Transform>(components.at("Transform"));
        world.set_local(e, transform.translation, transform.rotation, transform.scale);
    }
    for (const auto& [type_name, value] : components.items()) {
        if (type_name == "Name" || type_name == "Transform") {
            continue;
        }
        if (add_component(world.registry(), e, type_name)) {
            entt::meta_any component = component_by_name(world.registry(), e, type_name);
            core::reflect::apply_json(component, value);
        }
    }
    std::size_t child_index = 0;
    if (node.contains("children")) {
        for (const auto& child : node.at("children")) {
            restore_subtree(world, child, e, child_index++, fresh_guids);
        }
    }
    return e;
}

} // namespace f3d::scene
