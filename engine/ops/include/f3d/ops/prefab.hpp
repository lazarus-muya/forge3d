// SPDX-License-Identifier: Apache-2.0
#pragma once

// Prefabs (phases.md P3.4). A prefab is a reusable entity subtree saved as a
// "*.f3dprefab" JSON file in the project:
//
//   { "format": "forge3d.prefab", "version": 1, "name": "Lamp",
//     "root": { "node": "<guid>", "components": { "Name": ..., "Transform": ... },
//               "children": [ <node>, ... ] } }
//
// A node may instead reference another prefab ("prefab": "<VFS path>", with its
// "overrides"): a nested instance, one level deep. Node ids are stable: instances
// map their entities to nodes with PrefabLink, so the prefab can change without
// losing the mapping.
//
// An instance's root has a PrefabInstance: the prefab's path and the per-property
// overrides (recorded by entity.set_property when a linked entity is edited).
// Syncing an instance to its prefab copies every property except the overridden
// ones and the root's own Name/Transform (where the instance is placed), creates
// entities for new nodes and removes those whose node is gone. Entities the user
// added to an instance (no PrefabLink) are kept.

#include "f3d/ops/registry.hpp"

#include <entt/entity/entity.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::core::vfs {
class FileSystem;
} // namespace f3d::core::vfs

namespace f3d::ops {

namespace prefab {

inline constexpr const char* kFormat = "forge3d.prefab";
inline constexpr const char* kExtension = ".f3dprefab";

/// The prefab document for the subtree of `root`. Node ids come from PrefabLink
/// where the entities have one (applying an instance), else from entity GUIDs.
[[nodiscard]] nlohmann::json make(const scene::World& world, entt::entity root);

/// Reads and validates a prefab file.
[[nodiscard]] std::optional<nlohmann::json> load(const core::vfs::FileSystem& files, std::string_view path,
                                                 std::string* error = nullptr);

/// The nearest entity at or above `e` with a PrefabInstance (null if none).
[[nodiscard]] entt::entity instance_root(const scene::World& world, entt::entity e);

/// Every instance root in the world whose prefab is `path`, in scene order.
[[nodiscard]] std::vector<entt::entity> instances_of(const scene::World& world, std::string_view path);

/// Creates an instance of `prefab` (from file `path`) under `parent` at sibling
/// `index`; the root gets GUID `root_guid` (null: generated). Nested prefabs are
/// loaded through `files`. Returns null (and sets `error`) on failure.
entt::entity instantiate(scene::World& world, const core::vfs::FileSystem* files, const nlohmann::json& prefab,
                         const std::string& path, entt::entity parent, std::size_t index, const core::Guid& root_guid,
                         std::string* error = nullptr);

/// Brings the instance rooted at `root` in line with `prefab` (see the header comment).
void sync(scene::World& world, const core::vfs::FileSystem* files, entt::entity root, const nlohmann::json& prefab);

} // namespace prefab

/// Registers `prefab.create_from_selection`, `prefab.instantiate`,
/// `prefab.apply_overrides`, `prefab.revert_overrides` and `prefab.update_instances`.
/// They need Context::files.
void register_prefab_ops(OpRegistry& registry);

/// Called by entity.set_property: records an edit of an instance entity as an
/// override of its instance (returns the instance root and its overrides before,
/// for undo), or does nothing for entities outside instances.
struct OverrideUndo {
    core::Guid root;
    nlohmann::json previous; ///< the overrides before, as JSON (null: nothing recorded)
};
OverrideUndo record_override(scene::World& world, entt::entity e, const std::string& path, const nlohmann::json& value);
void restore_overrides(scene::World& world, const OverrideUndo& undo);

} // namespace f3d::ops
