// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/ops/registry.hpp"

namespace f3d::ops {

/// Registers the selection operations: `selection.set`, `selection.add`,
/// `selection.remove`. Idempotent per registry.
void register_selection_ops(OpRegistry& registry);

} // namespace f3d::ops
