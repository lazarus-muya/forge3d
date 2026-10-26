# SPDX-License-Identifier: Apache-2.0
# All HLSL shaders, compiled by DXC (cmake/F3DShaders.cmake). Target: f3d_shaders.
file(GLOB F3D_SHADER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.hlsl)
f3d_compile_shaders(f3d_shaders ${F3D_SHADER_SOURCES})
