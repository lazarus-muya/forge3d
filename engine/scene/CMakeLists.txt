# SPDX-License-Identifier: Apache-2.0
find_package(EnTT CONFIG REQUIRED)

f3d_add_module(f3d_scene
    SOURCES
        src/layers.cpp
        src/reflect.cpp
        src/selection.cpp
        src/sky.cpp
        src/snapshot.cpp
        src/world.cpp
    DEPS f3d_core
    PUBLIC_LIBS EnTT::EnTT)
