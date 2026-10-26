# SPDX-License-Identifier: Apache-2.0
find_package(zstd CONFIG REQUIRED)

f3d_add_module(f3d_io
    SOURCES
        src/archive.cpp
        src/archive_backend.cpp
        src/forge_file.cpp
        src/world_chunks.cpp
        src/module.cpp
    DEPS f3d_world
    PRIVATE_LIBS zstd::libzstd)
