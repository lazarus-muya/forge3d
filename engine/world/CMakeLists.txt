# SPDX-License-Identifier: Apache-2.0
find_package(zstd CONFIG REQUIRED)
f3d_add_module(f3d_world
    SOURCES src/module.cpp src/scatter.cpp src/scatter_meshes.cpp src/scatter_paint.cpp src/spline.cpp src/water.cpp src/world_ops.cpp
    DEPS f3d_terrain
    PRIVATE_LIBS zstd::libzstd)
