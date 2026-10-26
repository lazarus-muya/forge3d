// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/builtin.hpp"

#include "f3d/ops/entity_ops.hpp"
#include "f3d/ops/prefab.hpp"
#include "f3d/ops/selection_ops.hpp"
#include "f3d/scene/reflect.hpp"

namespace f3d::ops {

void register_builtin_ops(OpRegistry& registry) {
    scene::register_types();
    register_core_ops(registry);
    register_selection_ops(registry);
    register_layer_ops(registry);
    register_bookmark_ops(registry);
    register_prefab_ops(registry);
}

} // namespace f3d::ops
