# SPDX-License-Identifier: Apache-2.0
f3d_add_module(f3d_render_raster
    SOURCES
        src/atmosphere.cpp
        src/capture.cpp
        src/fog.cpp
        src/lights.cpp
        src/material_preview.cpp
        src/post_process.cpp
        src/render_settings.cpp
        src/residency.cpp
        src/shadows.cpp
        src/sky.cpp
        src/thumbnail_renderer.cpp
        src/raster_engine.cpp
        src/raster_terrain.cpp src/raster_water.cpp
    DEPS f3d_render_rhi f3d_render_upscale f3d_world f3d_modeling)
