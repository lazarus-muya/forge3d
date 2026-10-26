# SPDX-License-Identifier: Apache-2.0
#
# Diligent Engine, fetched with FetchContent at pinned commits (vcpkg has no
# Diligent port; see docs/progress-log.md, P0.1/P0.3).
#
# Provides:
#   Diligent-* targets from DiligentCore (D3D12 + Vulkan backends only)
#   Diligent::ImGui   Diligent's ImGui renderer (ImGuiImplDiligent /
#                     ImGuiDiligentRenderer) compiled against vcpkg's imgui,
#                     so there is exactly one imgui in the build.
#   Diligent::Engine  engine factories for the enabled backends
#
# Only f3d_render_rhi (and throw-away spikes) may link these (F3DLayering).

include_guard(GLOBAL)
include(FetchContent)

# DiligentCore / DiligentTools master as of 2026-10-14. The last release tag
# (v2.5.6, 2024-09) predates imgui 1.92's texture API, which vcpkg's imgui uses.
set(F3D_DILIGENT_CORE_COMMIT 199d9f3cc18723e7eafc5986d38b81e0995245ed)
set(F3D_DILIGENT_TOOLS_COMMIT 858b0380b0a43f5ab10a0c73c7f1ac83f211b08d)

function(_f3d_diligent_force_option name value)
    set(${name} ${value} CACHE BOOL "Set by forge3d (cmake/F3DDiligent.cmake)" FORCE)
endfunction()

_f3d_diligent_force_option(DILIGENT_NO_OPENGL ON)
_f3d_diligent_force_option(DILIGENT_NO_DIRECT3D11 ON)
_f3d_diligent_force_option(DILIGENT_NO_METAL ON)
_f3d_diligent_force_option(DILIGENT_NO_WEBGPU ON)
_f3d_diligent_force_option(DILIGENT_NO_ARCHIVER ON)
_f3d_diligent_force_option(DILIGENT_NO_RENDER_STATE_PACKAGER ON)
_f3d_diligent_force_option(DILIGENT_NO_FORMAT_VALIDATION ON)
_f3d_diligent_force_option(DILIGENT_BUILD_TESTS OFF)
_f3d_diligent_force_option(DILIGENT_INSTALL_CORE OFF)
_f3d_diligent_force_option(DILIGENT_INSTALL_PDB OFF)
if(NOT WIN32)
    _f3d_diligent_force_option(DILIGENT_NO_DIRECT3D12 ON)
endif()

# SPIRV-Cross test files exceed MAX_PATH on Windows. Git's environment config
# applies to the submodule clones as well, without touching the user's config.
if(WIN32)
    set(ENV{GIT_CONFIG_COUNT} 1)
    set(ENV{GIT_CONFIG_KEY_0} core.longpaths)
    set(ENV{GIT_CONFIG_VALUE_0} true)
endif()

FetchContent_Declare(DiligentCore
    GIT_REPOSITORY https://github.com/DiligentGraphics/DiligentCore.git
    GIT_TAG ${F3D_DILIGENT_CORE_COMMIT}
    GIT_SUBMODULES
        ThirdParty/glslang ThirdParty/SPIRV-Cross ThirdParty/SPIRV-Tools ThirdParty/SPIRV-Headers
        ThirdParty/Vulkan-Headers ThirdParty/volk ThirdParty/xxHash
    GIT_PROGRESS OFF
    # DiligentTools' headers include "../../../DiligentCore/...", so both
    # repositories must be siblings named exactly DiligentCore / DiligentTools.
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/DiligentCore
    SYSTEM
    EXCLUDE_FROM_ALL) # build only the Diligent targets forge3d links

FetchContent_Declare(DiligentTools
    GIT_REPOSITORY https://github.com/DiligentGraphics/DiligentTools.git
    GIT_TAG ${F3D_DILIGENT_TOOLS_COMMIT}
    GIT_SUBMODULES ""
    GIT_PROGRESS OFF
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/DiligentTools
    SOURCE_SUBDIR f3d-not-built # only the ImGui renderer sources are used (below)
    SYSTEM)

message(STATUS "forge3d: fetching Diligent (core ${F3D_DILIGENT_CORE_COMMIT}, tools ${F3D_DILIGENT_TOOLS_COMMIT})")
FetchContent_MakeAvailable(DiligentCore DiligentTools)

find_package(imgui CONFIG REQUIRED)

set(_f3d_dt ${diligenttools_SOURCE_DIR})
add_library(forge3d_diligent_imgui STATIC
    ${_f3d_dt}/Imgui/src/ImGuiDiligentRenderer.cpp
    ${_f3d_dt}/Imgui/src/ImGuiImplDiligent.cpp)
target_include_directories(forge3d_diligent_imgui SYSTEM PUBLIC ${_f3d_dt}/Imgui/interface)
target_link_libraries(forge3d_diligent_imgui PUBLIC
    imgui::imgui
    Diligent-GraphicsEngineInterface
    Diligent-GraphicsAccessories
    Diligent-GraphicsTools
    Diligent-Common)
target_compile_features(forge3d_diligent_imgui PUBLIC cxx_std_20)
set_target_properties(forge3d_diligent_imgui PROPERTIES FOLDER DiligentTools)

add_library(forge3d_diligent INTERFACE)
if(TARGET Diligent-GraphicsEngineD3D12-static)
    target_link_libraries(forge3d_diligent INTERFACE Diligent-GraphicsEngineD3D12-static)
    target_compile_definitions(forge3d_diligent INTERFACE F3D_DILIGENT_D3D12=1)
endif()
if(TARGET Diligent-GraphicsEngineVk-static)
    target_link_libraries(forge3d_diligent INTERFACE Diligent-GraphicsEngineVk-static)
    target_compile_definitions(forge3d_diligent INTERFACE F3D_DILIGENT_VULKAN=1)
endif()
target_link_libraries(forge3d_diligent INTERFACE
    Diligent-GraphicsTools Diligent-GraphicsAccessories Diligent-Common Diligent-PlatformInterface)

# The names contain "Diligent", so cmake/F3DLayering.cmake keeps them inside f3d_render_rhi.
add_library(Diligent::Engine ALIAS forge3d_diligent)
add_library(Diligent::ImGui ALIAS forge3d_diligent_imgui)
