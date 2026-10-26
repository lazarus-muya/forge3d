// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/ops/registry.hpp"

namespace f3d::ops {

/// Registers the core entity operations and their reflected parameter types:
/// `entity.create`, `entity.delete`, `entity.set_property`, `entity.reparent`,
/// `entity.duplicate`, `entity.add_component`, `entity.remove_component`.
void register_core_ops(OpRegistry& registry);

/// `entity.add_component` / `entity.remove_component` (part of register_core_ops).
void register_component_ops(OpRegistry& registry);

} // namespace f3d::ops
