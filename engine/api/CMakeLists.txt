# SPDX-License-Identifier: Apache-2.0
find_package(ixwebsocket CONFIG REQUIRED)

f3d_add_module(f3d_api
    SOURCES src/host.cpp src/module.cpp src/rpc.cpp src/rpc_server.cpp
    DEPS f3d_assets f3d_io f3d_render_raster f3d_terrain f3d_world)
# Plugins are loaded with dlopen on Linux and macOS; the RPC server is a WebSocket server.
target_link_libraries(f3d_api PRIVATE ${CMAKE_DL_LIBS} ixwebsocket::ixwebsocket)
