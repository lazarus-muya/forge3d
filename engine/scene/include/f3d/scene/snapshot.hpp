// SPDX-License-Identifier: Apache-2.0
#pragma once

// Entity (sub)tree snapshots built from reflection (f3d/core/reflect.hpp).
// Used by the delete/duplicate operations to restore a subtree exactly on undo
// and, later, by the .forge scene serialiser (P2.6).

#include "f3d/core/guid.hpp"

#include <entt/entity/entity.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>

namespace f3d::scene {

class World;

/// Serialises `root` and its descendants: guid, name, components and children.
[[nodiscard]] nlohmann::json snapshot_subtree(const World& world, entt::entity root);

/// Serialises the scene: { "roots": [ <subtree>, ... ], "layers": [ <LayerInfo>, ... ],
/// "bookmarks": [ <CameraBookmark>, ... ] }. The selection is not included.
[[nodiscard]] nlohmann::json snapshot_world(const World& world);

/// Replaces the scene with a snapshot_world() result (clears the world first).
/// Returns false, with `error` set, on malformed input; the world then holds
/// whatever was restored before the problem.
bool restore_world(World& world, const nlohmann::json& snapshot, std::string* error = nullptr);

/// Recreates a subtree under `parent` at sibling `index`. With `fresh_guids` new
/// GUIDs are generated (duplicate; `root_guid`, if not null, is used for the
/// root); otherwise the stored GUIDs are reused (undo). Returns the new root, or
/// null on failure.
entt::entity restore_subtree(World& world, const nlohmann::json& node, entt::entity parent, std::size_t index,
                             bool fresh_guids, const core::Guid& root_guid = {});

} // namespace f3d::scene
