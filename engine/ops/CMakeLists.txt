# SPDX-License-Identifier: Apache-2.0
f3d_add_module(f3d_ops
    SOURCES
        src/bookmark_ops.cpp
        src/builtin.cpp
        src/component_ops.cpp
        src/entity_ops.cpp
        src/layer_ops.cpp
        src/prefab_ops.cpp
        src/module.cpp
        src/registry.cpp
        src/script.cpp
        src/selection_ops.cpp
        src/stack.cpp
    DEPS f3d_scene)
