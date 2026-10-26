# SPDX-License-Identifier: Apache-2.0
find_package(zstd CONFIG REQUIRED)
f3d_add_module(f3d_terrain
    SOURCES src/builtin_textures.cpp src/erosion.cpp src/heightfield.cpp src/layers.cpp src/module.cpp src/sculpt.cpp src/terrain_ops.cpp
    DEPS f3d_assets
    PRIVATE_LIBS zstd::libzstd)
