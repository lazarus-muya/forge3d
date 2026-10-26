// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/ops/registry.hpp"

namespace f3d::ops {

/// Registers the layer operations: `layer.create`, `layer.delete`, `layer.rename`,
/// `layer.set_visible`, `layer.set_locked`, `layer.assign`.
void register_layer_ops(OpRegistry& registry);

/// Registers the camera-bookmark operations: `bookmark.save`, `bookmark.delete`,
/// `bookmark.recall` (transient: not recorded in the history).
void register_bookmark_ops(OpRegistry& registry);

/// Registers every operation the engine provides (entity, selection, layer,
/// bookmark, prefab). The editor, the CLI and the RPC server call this once per registry.
void register_builtin_ops(OpRegistry& registry);

} // namespace f3d::ops
