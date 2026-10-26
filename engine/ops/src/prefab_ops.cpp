// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/prefab.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/reflect_json.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace f3d::ops {

using core::Guid;
using nlohmann::json;
using scene::World;

namespace prefab {
namespace {

/// Components a prefab stores: everything reflected except the bookkeeping ones
/// (prefab links/instances) and scene-only ones (layers).
bool stored(std::string_view type) {
    return type != "PrefabInstance" && type != "PrefabLink" && type != "Layer";
}

json components_of(const World& world, entt::entity e) {
    json out = json::object();
    scene::for_each_component(const_cast<entt::registry&>(world.registry()), e, [&](entt::meta_any c) {
        const char* name = c.type().name();
        if (name && stored(name)) {
            core::reflect::value_to_json(out[name], c);
        }
    });
    return out;
}

Guid node_id(const World& world, entt::entity e, bool nested_root) {
    if (const auto* link = world.registry().try_get<scene::PrefabLink>(e)) {
        const Guid id = nested_root ? link->outer_node : link->node;
        if (!id.is_null()) {
            return id;
        }
    }
    return world.guid(e);
}

json make_node(const World& world, entt::entity e, bool top) {
    const auto& registry = world.registry();
    const auto* instance = registry.try_get<scene::PrefabInstance>(e);
    if (instance && !top) {
        // A nested instance: referenced, not copied (its contents belong to its own prefab).
        json node;
        node["node"] = node_id(world, e, true).to_string();
        node["prefab"] = instance->prefab.path;
        json components = json::object();
        const json all = components_of(world, e);
        for (const char* kept : {"Name", "Transform"}) {
            if (all.contains(kept)) {
                components[kept] = all.at(kept);
            }
        }
        node["components"] = std::move(components);
        node["overrides"] = core::reflect::to_json(instance->overrides);
        return node;
    }
    json node;
    node["node"] = node_id(world, e, false).to_string();
    node["components"] = components_of(world, e);
    node["children"] = json::array();
    for (const entt::entity child : world.children(e)) {
        node["children"].push_back(make_node(world, child, false));
    }
    return node;
}

/// Writes the fields of `components` onto `e` (adding optional components),
/// skipping the fields `skip(component.field)` says to keep.
template <class Skip>
void apply_components(World& world, entt::entity e, const json& components, Skip&& skip) {
    for (const auto& [type, fields] : components.items()) {
        if (!stored(type)) {
            continue;
        }
        entt::meta_any component = scene::component_by_name(world.registry(), e, type);
        if (!component) {
            if (!scene::add_component(world.registry(), e, type)) {
                continue;
            }
            component = scene::component_by_name(world.registry(), e, type);
        }
        for (const auto& [field, value] : fields.items()) {
            if (skip(type + "." + field)) {
                continue;
            }
            const entt::meta_data data = core::reflect::field_by_name(component.type(), field);
            if (data) {
                try {
                    data.set(component, core::reflect::value_from_json(value, data.type()));
                } catch (const std::exception& ex) {
                    F3D_LOG_WARN("Prefab: {}.{}: {}", type, field, ex.what());
                }
            }
        }
        if (type == "Transform") {
            world.mark_dirty(e);
        }
    }
}

entt::entity create_node(World& world, const core::vfs::FileSystem* files, const json& node, entt::entity parent,
                         std::size_t index, const Guid& guid, int depth, std::string* error);
std::map<Guid, entt::entity> instance_nodes(const World& world, entt::entity root);

/// Writes the instance's recorded override values onto its entities.
void apply_override_values(World& world, entt::entity root) {
    const auto& instance = world.registry().get<scene::PrefabInstance>(root);
    const std::map<Guid, entt::entity> nodes = instance_nodes(world, root);
    for (const scene::PrefabOverride& o : instance.overrides) {
        const auto it = nodes.find(o.node);
        const auto dot = o.path.find('.');
        if (it == nodes.end() || dot == std::string::npos) {
            continue;
        }
        const json value = json::parse(o.value, nullptr, false);
        if (value.is_discarded()) {
            continue;
        }
        json components = json::object();
        components[o.path.substr(0, dot)][o.path.substr(dot + 1)] = value;
        apply_components(world, it->second, components, [](const std::string&) { return false; });
    }
}

/// Instantiates a nested prefab node: an instance of another prefab placed by the outer one.
entt::entity create_nested(World& world, const core::vfs::FileSystem* files, const json& node, entt::entity parent,
                           std::size_t index, int depth, std::string* error) {
    const std::string path = node.value("prefab", std::string());
    if (depth >= 1) {
        if (error) {
            *error = "prefabs nest one level deep only (" + path + " is nested twice)";
        }
        return entt::null;
    }
    if (!files) {
        if (error) {
            *error = "no project files to load " + path;
        }
        return entt::null;
    }
    const std::optional<json> inner = load(*files, path, error);
    if (!inner) {
        return entt::null;
    }
    const entt::entity e = create_node(world, files, inner->at("root"), parent, index, Guid{}, depth + 1, error);
    if (e == entt::null) {
        return entt::null;
    }
    auto& instance = world.registry().emplace_or_replace<scene::PrefabInstance>(e);
    instance.prefab.path = path;
    if (node.contains("overrides")) {
        instance.overrides = core::reflect::from_json<std::vector<scene::PrefabOverride>>(node.at("overrides"));
    }
    world.registry().get<scene::PrefabLink>(e).outer_node =
        Guid::parse(node.at("node").get<std::string>()).value_or(Guid{});
    // The outer prefab places it (Name/Transform), then the nested overrides apply.
    apply_components(world, e, node.value("components", json::object()), [](const std::string&) { return false; });
    sync(world, files, e, *inner);
    apply_override_values(world, e);
    return e;
}

entt::entity create_node(World& world, const core::vfs::FileSystem* files, const json& node, entt::entity parent,
                         std::size_t index, const Guid& guid, int depth, std::string* error) {
    if (node.contains("prefab")) {
        return create_nested(world, files, node, parent, index, depth, error);
    }
    const json& components = node.value("components", json::object());
    std::string name = "Entity";
    if (components.contains("Name")) {
        name = components.at("Name").value("value", name);
    }
    const entt::entity e = world.create(name, parent, guid, index);
    if (e == entt::null) {
        if (error) {
            *error = "cannot create entity " + guid.to_string();
        }
        return entt::null;
    }
    apply_components(world, e, components, [](const std::string&) { return false; });
    world.registry().emplace<scene::PrefabLink>(
        e, scene::PrefabLink{Guid::parse(node.at("node").get<std::string>()).value_or(Guid{}), Guid{}});
    std::size_t child_index = 0;
    for (const json& child : node.value("children", json::array())) {
        if (create_node(world, files, child, e, child_index++, Guid{}, depth, error) == entt::null) {
            return entt::null;
        }
    }
    return e;
}

/// Entities of the instance rooted at `root`, by node id: its own linked entities
/// and the roots of nested instances (by their outer node); nested contents and
/// unlinked additions are left out.
std::map<Guid, entt::entity> instance_nodes(const World& world, entt::entity root) {
    std::map<Guid, entt::entity> out;
    const auto& registry = world.registry();
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        const auto* link = registry.try_get<scene::PrefabLink>(e);
        if (e != root && registry.all_of<scene::PrefabInstance>(e)) {
            if (link && !link->outer_node.is_null()) {
                out[link->outer_node] = e;
            }
            continue; // its contents are synced by its own prefab
        }
        if (link) {
            out[link->node] = e;
        }
        for (const entt::entity child : world.children(e)) {
            stack.push_back(child);
        }
    }
    return out;
}

} // namespace

json make(const World& world, entt::entity root) {
    json out;
    out["format"] = kFormat;
    out["version"] = 1;
    out["name"] = world.name(root);
    out["root"] = make_node(world, root, true);
    return out;
}

std::optional<json> load(const core::vfs::FileSystem& files, std::string_view path, std::string* error) {
    const auto fail = [&](std::string why) -> std::optional<json> {
        if (error) {
            *error = std::string(path) + ": " + std::move(why);
        }
        return std::nullopt;
    };
    const auto text = files.read_text(path);
    if (!text) {
        return fail("cannot read the prefab");
    }
    json doc = json::parse(*text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || doc.value("format", std::string()) != kFormat) {
        return fail("not a forge3d prefab");
    }
    if (doc.value("version", 0) != 1) {
        return fail("unsupported prefab version");
    }
    if (!doc.contains("root") || !doc.at("root").is_object() || !doc.at("root").contains("node")) {
        return fail("no root node");
    }
    return doc;
}

entt::entity instance_root(const World& world, entt::entity e) {
    for (entt::entity n = e; n != entt::null; n = world.parent(n)) {
        if (world.registry().all_of<scene::PrefabInstance>(n)) {
            return n;
        }
    }
    return entt::null;
}

std::vector<entt::entity> instances_of(const World& world, std::string_view path) {
    std::vector<entt::entity> out;
    for (const entt::entity e : world.depth_first()) {
        if (const auto* instance = world.registry().try_get<scene::PrefabInstance>(e);
            instance && instance->prefab.path == path) {
            out.push_back(e);
        }
    }
    return out;
}

entt::entity instantiate(World& world, const core::vfs::FileSystem* files, const json& prefab, const std::string& path,
                         entt::entity parent, std::size_t index, const Guid& root_guid, std::string* error) {
    F3D_ZONE;
    const json& root_node = prefab.at("root");
    if (root_node.contains("prefab")) {
        if (error) {
            *error = path + ": a prefab's root can't itself be a nested prefab";
        }
        return entt::null;
    }
    const entt::entity root = create_node(world, files, root_node, parent, index, root_guid, 0, error);
    if (root == entt::null) {
        return entt::null;
    }
    world.registry().emplace_or_replace<scene::PrefabInstance>(root).prefab.path = path;
    return root;
}

void sync(World& world, const core::vfs::FileSystem* files, entt::entity root, const json& prefab) {
    F3D_ZONE;
    const auto& instance = world.registry().get<scene::PrefabInstance>(root);
    std::set<std::pair<Guid, std::string>> overridden;
    for (const scene::PrefabOverride& o : instance.overrides) {
        overridden.emplace(o.node, o.path);
    }
    const std::map<Guid, entt::entity> existing = instance_nodes(world, root);
    std::set<entt::entity> visited;

    // Depth-first over the prefab, creating what's missing and updating the rest.
    struct Item {
        const json* node;
        entt::entity parent;
        bool is_root;
    };
    std::vector<Item> stack{{&prefab.at("root"), world.parent(root), true}};
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const json& node = *item.node;
        const Guid id = Guid::parse(node.at("node").get<std::string>()).value_or(Guid{});
        const auto it = existing.find(id);
        entt::entity e = it == existing.end() ? entt::null : it->second;
        if (item.is_root) {
            e = root;
        }
        if (e == entt::null) {
            std::string error;
            e = create_node(world, files, node, item.parent, World::kAppend, Guid{}, 0, &error);
            if (e == entt::null) {
                F3D_LOG_WARN("Prefab sync: {}", error);
                continue;
            }
            // A new node comes complete (children included): mark its subtree as handled.
            for (const entt::entity created : world.subtree(e)) {
                visited.insert(created);
            }
            continue;
        }
        visited.insert(e);
        const auto keep = [&](const std::string& path) {
            if (item.is_root && (path.starts_with("Name.") || path.starts_with("Transform."))) {
                return true; // the instance's own placement
            }
            return overridden.contains({id, path});
        };
        const json& components = node.value("components", json::object());
        if (node.contains("prefab")) {
            apply_components(world, e, components, keep); // a nested root: only its placement
            continue;
        }
        apply_components(world, e, components, keep);
        // Optional components the prefab no longer has.
        for (const std::string_view type : scene::addable_components()) {
            if (!components.contains(std::string(type))) {
                scene::remove_component(world.registry(), e, type);
            }
        }
        // Pointers into the document itself: they outlive this iteration (node.value() would copy).
        if (const auto children = node.find("children"); children != node.end() && children->is_array()) {
            for (auto child = children->rbegin(); child != children->rend(); ++child) {
                stack.push_back({&*child, e, false});
            }
        }
    }
    // Entities whose node is gone from the prefab.
    for (const auto& [id, e] : existing) {
        if (!visited.contains(e) && world.valid(e)) {
            world.destroy(e);
        }
    }
}

} // namespace prefab

// --- Override recording (entity.set_property) ----------------------------------------------------

OverrideUndo record_override(World& world, entt::entity e, const std::string& path, const json& value) {
    OverrideUndo undo;
    const auto* link = world.registry().try_get<scene::PrefabLink>(e);
    if (!link) {
        return undo; // not part of an instance (or an addition to one)
    }
    const bool placement = path.starts_with("Name.") || path.starts_with("Transform.");
    entt::entity root = prefab::instance_root(world, e);
    Guid node = link->node;
    if (root == e && placement) {
        // The root's placement belongs to the instance; for a nested root it's an
        // override of the outer instance.
        if (link->outer_node.is_null() || world.parent(e) == entt::null) {
            return undo;
        }
        root = prefab::instance_root(world, world.parent(e));
        node = link->outer_node;
    }
    if (root == entt::null) {
        return undo;
    }
    auto& instance = world.registry().get<scene::PrefabInstance>(root);
    undo.root = world.guid(root);
    undo.previous = core::reflect::to_json(instance.overrides);
    auto it = std::find_if(instance.overrides.begin(), instance.overrides.end(),
                           [&](const scene::PrefabOverride& o) { return o.node == node && o.path == path; });
    if (it == instance.overrides.end()) {
        instance.overrides.push_back({node, path, value.dump()});
    } else {
        it->value = value.dump();
    }
    return undo;
}

void restore_overrides(World& world, const OverrideUndo& undo) {
    if (undo.root.is_null() || undo.previous.is_null()) {
        return;
    }
    if (const entt::entity root = world.find(undo.root); root != entt::null) {
        if (auto* instance = world.registry().try_get<scene::PrefabInstance>(root)) {
            instance->overrides = core::reflect::from_json<std::vector<scene::PrefabOverride>>(undo.previous);
        }
    }
}

// --- Operations -----------------------------------------------------------------------------------

namespace {

// Parameter structs get globally unique names: EnTT identifies types by their
// pretty name, and MSVC prints every anonymous namespace the same way, so two
// TUs' "CreateParams" would share one meta type.
struct PrefabCreateParams {
    Guid entity;
    std::string path; ///< VFS path of the .f3dprefab to write
};
struct PrefabInstantiateParams {
    std::string prefab; ///< VFS path
    Guid parent;
    glm::vec3 position{0.0f};
    Guid guid; ///< optional root GUID
};
struct PrefabEntityParams {
    Guid entity;
};
struct PrefabRevertParams {
    Guid entity;
    Guid node;        ///< null: every override
    std::string path; ///< "": every property of `node`
};
struct PrefabUpdateParams {
    std::string prefab;
};

void register_param_types() {
    entt::meta_factory<PrefabCreateParams>{}
        .type("prefab.create.params")
        .data<&PrefabCreateParams::entity>("entity")
        .data<&PrefabCreateParams::path>("path");
    entt::meta_factory<PrefabInstantiateParams>{}
        .type("prefab.instantiate.params")
        .data<&PrefabInstantiateParams::prefab>("prefab")
        .data<&PrefabInstantiateParams::parent>("parent")
        .data<&PrefabInstantiateParams::position>("position")
        .data<&PrefabInstantiateParams::guid>("guid");
    entt::meta_factory<PrefabEntityParams>{}.type("prefab.entity.params").data<&PrefabEntityParams::entity>("entity");
    entt::meta_factory<PrefabRevertParams>{}
        .type("prefab.revert.params")
        .data<&PrefabRevertParams::entity>("entity")
        .data<&PrefabRevertParams::node>("node")
        .data<&PrefabRevertParams::path>("path");
    entt::meta_factory<PrefabUpdateParams>{}.type("prefab.update.params").data<&PrefabUpdateParams::prefab>("prefab");
}

/// An exact copy of entity subtrees, restored on undo (GUIDs, order and all).
class SubtreeBackup {
public:
    void capture(const World& world, entt::entity e) {
        Saved saved;
        saved.guid = world.guid(e);
        saved.snapshot = scene::snapshot_subtree(world, e);
        const entt::entity parent = world.parent(e);
        saved.parent = parent == entt::null ? Guid{} : world.guid(parent);
        saved.index = world.sibling_index(e);
        saved_.push_back(std::move(saved));
    }
    void restore(World& world) const {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            if (const entt::entity current = world.find(it->guid); current != entt::null) {
                world.destroy(current);
            }
            const entt::entity parent = it->parent.is_null() ? entt::null : world.find(it->parent);
            scene::restore_subtree(world, it->snapshot, parent, it->index, false);
        }
    }
    [[nodiscard]] std::size_t bytes() const {
        std::size_t n = 0;
        for (const Saved& s : saved_) {
            n += s.snapshot.dump().size();
        }
        return n;
    }
    [[nodiscard]] bool empty() const noexcept { return saved_.empty(); }
    void clear() { saved_.clear(); }

private:
    struct Saved {
        Guid guid;
        json snapshot;
        Guid parent;
        std::size_t index = 0;
    };
    std::vector<Saved> saved_;
};

/// The previous contents of a file, restored on undo.
struct FileBackup {
    std::string path;
    std::optional<std::string> text; ///< nullopt: the file didn't exist

    void capture(const core::vfs::FileSystem& files, std::string p) {
        path = std::move(p);
        text = files.read_text(path);
    }
    void restore(core::vfs::FileSystem& files) const {
        if (text) {
            files.write_text(path, *text);
        } else {
            files.remove(path);
        }
    }
};

void hierarchy_changed(Context& context, const Guid& guid) {
    context.changes.emit({ChangeKind::HierarchyChanged, guid});
}

/// Turns the subtree of `root` into an instance of `path` (links every entity to
/// its own GUID as the node id; nested instances keep their contents).
void link_as_instance(World& world, entt::entity root, const std::string& path) {
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        auto& registry = world.registry();
        if (e != root && registry.all_of<scene::PrefabInstance>(e)) {
            registry.get_or_emplace<scene::PrefabLink>(e).outer_node = world.guid(e);
            continue;
        }
        registry.emplace_or_replace<scene::PrefabLink>(e, scene::PrefabLink{world.guid(e), Guid{}});
        for (const entt::entity child : world.children(e)) {
            stack.push_back(child);
        }
    }
    world.registry().emplace_or_replace<scene::PrefabInstance>(root).prefab.path = path;
}

class PrefabCreateOp final : public OperationBase<PrefabCreateParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "prefab.create_from_selection"; }
    [[nodiscard]] std::string display_name() const override { return "Create prefab " + params.path; }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        if (!context.files || e == entt::null || !params.path.ends_with(prefab::kExtension)) {
            return false;
        }
        if (context.world.registry().any_of<scene::PrefabInstance, scene::PrefabLink>(e)) {
            return false; // already part of an instance: apply its overrides instead
        }
        backup_.clear();
        backup_.capture(context.world, e);
        file_.capture(*context.files, params.path);
        if (!context.files->write_text(params.path, prefab::make(context.world, e).dump(2))) {
            return false;
        }
        link_as_instance(context.world, e, params.path);
        hierarchy_changed(context, params.entity);
        return true;
    }

    void undo(Context& context) override {
        backup_.restore(context.world);
        if (context.files) {
            file_.restore(*context.files);
        }
        hierarchy_changed(context, params.entity);
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + backup_.bytes() + (file_.text ? file_.text->size() : 0);
    }

private:
    SubtreeBackup backup_;
    FileBackup file_;
};

class PrefabInstantiateOp final : public OperationBase<PrefabInstantiateParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "prefab.instantiate"; }
    [[nodiscard]] std::string display_name() const override { return "Instantiate " + params.prefab; }

    bool execute(Context& context) override {
        const entt::entity parent = params.parent.is_null() ? entt::null : context.world.find(params.parent);
        if (!params.parent.is_null() && parent == entt::null) {
            return false;
        }
        entt::entity root = entt::null;
        if (created_.is_null()) {
            if (!context.files) {
                return false;
            }
            std::string error;
            const std::optional<json> doc = prefab::load(*context.files, params.prefab, &error);
            if (!doc) {
                F3D_LOG_ERROR("prefab.instantiate: {}", error);
                return false;
            }
            root = prefab::instantiate(context.world, context.files, *doc, params.prefab, parent, World::kAppend,
                                       params.guid, &error);
            if (root == entt::null) {
                F3D_LOG_ERROR("prefab.instantiate: {}", error);
                return false;
            }
            const scene::Transform& t = context.world.transform(root);
            context.world.set_local(root, params.position, t.rotation, t.scale);
            created_ = context.world.guid(root);
            snapshot_ = scene::snapshot_subtree(context.world, root); // redo recreates the same GUIDs
        } else {
            root = scene::restore_subtree(context.world, snapshot_, parent, World::kAppend, false);
            if (root == entt::null) {
                return false;
            }
        }
        context.changes.emit({ChangeKind::EntityCreated, created_});
        return true;
    }

    void undo(Context& context) override {
        if (const entt::entity e = context.world.find(created_); e != entt::null) {
            context.world.destroy(e);
            context.changes.emit({ChangeKind::EntityDestroyed, created_});
        }
    }

    [[nodiscard]] std::size_t memory_size() const override { return sizeof(*this) + snapshot_.dump().size(); }

private:
    Guid created_;
    json snapshot_;
};

/// Base of the ops that sync instances: backs up every instance it touches.
template <class P>
class SyncingOp : public OperationBase<P> {
protected:
    bool sync_all(Context& context, const std::string& path, const std::optional<Guid>& except = std::nullopt) {
        std::string error;
        const std::optional<json> doc = prefab::load(*context.files, path, &error);
        if (!doc) {
            F3D_LOG_ERROR("{}: {}", this->id(), error);
            return false;
        }
        for (const entt::entity root : prefab::instances_of(context.world, path)) {
            if (except && context.world.guid(root) == *except) {
                continue;
            }
            backup_.capture(context.world, root);
            prefab::sync(context.world, context.files, root, *doc);
            hierarchy_changed(context, context.world.guid(root));
        }
        return true;
    }

    [[nodiscard]] std::size_t memory_size() const override { return sizeof(*this) + backup_.bytes(); }

    SubtreeBackup backup_;
};

class PrefabUpdateOp final : public SyncingOp<PrefabUpdateParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "prefab.update_instances"; }
    [[nodiscard]] std::string display_name() const override { return "Update instances of " + params.prefab; }

    bool execute(Context& context) override {
        if (!context.files || prefab::instances_of(context.world, params.prefab).empty()) {
            return false;
        }
        backup_.clear();
        return sync_all(context, params.prefab);
    }

    void undo(Context& context) override { backup_.restore(context.world); }
};

class PrefabApplyOp final : public SyncingOp<PrefabEntityParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "prefab.apply_overrides"; }
    [[nodiscard]] std::string display_name() const override { return "Apply overrides to " + path_; }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        const entt::entity root = e == entt::null ? entt::null : prefab::instance_root(context.world, e);
        if (!context.files || root == entt::null) {
            return false;
        }
        path_ = context.world.registry().get<scene::PrefabInstance>(root).prefab.path;
        backup_.clear();
        backup_.capture(context.world, root);
        file_.capture(*context.files, path_);
        // The instance as it is now becomes the prefab (its placement aside: the prefab
        // keeps its own root Name/Transform).
        json doc = prefab::make(context.world, root);
        if (const std::optional<json> old = prefab::load(*context.files, path_)) {
            for (const char* kept : {"Name", "Transform"}) {
                if (old->at("root").contains("components") && old->at("root").at("components").contains(kept)) {
                    doc["root"]["components"][kept] = old->at("root").at("components").at(kept);
                }
            }
            doc["name"] = old->value("name", doc.value("name", std::string()));
        }
        if (!context.files->write_text(path_, doc.dump(2))) {
            return false;
        }
        // Entities added to this instance are now nodes of the prefab; its overrides are applied.
        const entt::entity instance_root = root;
        for (const entt::entity x : context.world.subtree(instance_root)) {
            auto& registry = context.world.registry();
            if (x != instance_root && registry.all_of<scene::PrefabInstance>(x)) {
                // A nested instance directly in this one: its slot in the prefab.
                if (prefab::instance_root(context.world, context.world.parent(x)) == instance_root) {
                    auto& link = registry.get_or_emplace<scene::PrefabLink>(x);
                    if (link.outer_node.is_null()) {
                        link.outer_node = context.world.guid(x);
                    }
                }
                continue;
            }
            if (x != instance_root && prefab::instance_root(context.world, x) != instance_root) {
                continue; // inside a nested instance
            }
            (void)registry.get_or_emplace<scene::PrefabLink>(x, scene::PrefabLink{context.world.guid(x), {}});
        }
        context.world.registry().get<scene::PrefabInstance>(root).overrides.clear();
        hierarchy_changed(context, context.world.guid(root));
        return sync_all(context, path_, context.world.guid(root));
    }

    void undo(Context& context) override {
        backup_.restore(context.world);
        if (context.files) {
            file_.restore(*context.files);
        }
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return SyncingOp::memory_size() + (file_.text ? file_.text->size() : 0);
    }

private:
    std::string path_;
    FileBackup file_;
};

class PrefabRevertOp final : public SyncingOp<PrefabRevertParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "prefab.revert_overrides"; }
    [[nodiscard]] std::string display_name() const override { return "Revert overrides"; }

    bool execute(Context& context) override {
        const entt::entity e = context.world.find(params.entity);
        const entt::entity root = e == entt::null ? entt::null : prefab::instance_root(context.world, e);
        if (!context.files || root == entt::null) {
            return false;
        }
        auto& instance = context.world.registry().get<scene::PrefabInstance>(root);
        const auto matches = [&](const scene::PrefabOverride& o) {
            return (params.node.is_null() || o.node == params.node) && (params.path.empty() || o.path == params.path);
        };
        if (std::none_of(instance.overrides.begin(), instance.overrides.end(), matches)) {
            return false; // nothing to revert
        }
        std::string error;
        const std::optional<json> doc = prefab::load(*context.files, instance.prefab.path, &error);
        if (!doc) {
            F3D_LOG_ERROR("prefab.revert_overrides: {}", error);
            return false;
        }
        backup_.clear();
        backup_.capture(context.world, root);
        std::erase_if(instance.overrides, matches);
        prefab::sync(context.world, context.files, root, *doc);
        hierarchy_changed(context, context.world.guid(root));
        return true;
    }

    void undo(Context& context) override { backup_.restore(context.world); }
};

} // namespace

void register_prefab_ops(OpRegistry& registry) {
    register_param_types();
    registry.add({"prefab.create_from_selection", "Create Prefab", entt::resolve<PrefabCreateParams>()},
                 [] { return std::make_unique<PrefabCreateOp>(); });
    registry.add({"prefab.instantiate", "Instantiate Prefab", entt::resolve<PrefabInstantiateParams>()},
                 [] { return std::make_unique<PrefabInstantiateOp>(); });
    registry.add({"prefab.apply_overrides", "Apply Prefab Overrides", entt::resolve<PrefabEntityParams>()},
                 [] { return std::make_unique<PrefabApplyOp>(); });
    registry.add({"prefab.revert_overrides", "Revert Prefab Overrides", entt::resolve<PrefabRevertParams>()},
                 [] { return std::make_unique<PrefabRevertOp>(); });
    registry.add({"prefab.update_instances", "Update Prefab Instances", entt::resolve<PrefabUpdateParams>()},
                 [] { return std::make_unique<PrefabUpdateOp>(); });
}

} // namespace f3d::ops
