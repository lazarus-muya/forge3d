# SPDX-License-Identifier: Apache-2.0
find_package(fastgltf CONFIG REQUIRED)
find_package(ufbx CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(xatlas CONFIG REQUIRED)
find_package(bc7enc_rdo CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

f3d_add_module(f3d_assets
    SOURCES
        src/asset_database.cpp
        src/asset_index.cpp
        src/cook.cpp
        src/gltf.cpp
        src/material.cpp
        src/material_ops.cpp
        src/mesh.cpp
        src/mesh_library.cpp
        src/model_import.cpp
        src/pipeline.cpp
        src/raycast.cpp
        src/source_watcher.cpp
        src/texture.cpp
    DEPS f3d_ops
    PRIVATE_LIBS fastgltf::fastgltf ufbx::ufbx tinyobjloader::tinyobjloader meshoptimizer::meshoptimizer
                 xatlas::xatlas bc7enc_rdo::bc7enc_rdo unofficial::tinyexr::tinyexr unofficial::sqlite3::sqlite3)
