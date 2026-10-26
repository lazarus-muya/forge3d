# SPDX-License-Identifier: Apache-2.0
#
# HLSL shader build (phases.md P1.1). Every `shaders/<name>.<vs|ps|cs>.hlsl` is
# compiled by DXC into ${F3D_SHADER_BINARY_DIR}/<name>.<stage>.spv (Vulkan) and,
# on Windows, <name>.<stage>.dxil (D3D12). The same DXC and flags are used for
# hot reload at run time (f3d_shader_config.hpp, generated for f3d_render_rhi).

include_guard(GLOBAL)

find_package(directx-dxc CONFIG REQUIRED)

set(F3D_SHADER_SOURCE_DIR ${PROJECT_SOURCE_DIR}/shaders)
set(F3D_SHADER_BINARY_DIR ${PROJECT_BINARY_DIR}/shaders)
set(F3D_DXC ${DIRECTX_DXC_TOOL})

# Shared flags. Column-major matrices (DXC default) match GLM's memory layout.
set(F3D_DXC_FLAGS -E main -HV 2021 -O3 -WX -I ${F3D_SHADER_SOURCE_DIR})
# -fspv-reflect keeps HLSL semantics, which Diligent uses to map ATTRIBn inputs.
set(F3D_DXC_SPIRV_FLAGS -spirv -fspv-target-env=vulkan1.1 -fspv-reflect)

# f3d_compile_shaders(<target> <source>...): custom target building all outputs.
function(f3d_compile_shaders target)
    file(GLOB_RECURSE _includes CONFIGURE_DEPENDS ${F3D_SHADER_SOURCE_DIR}/*.hlsli)
    set(_outputs)
    foreach(src IN LISTS ARGN)
        get_filename_component(_file ${src} NAME)
        string(REGEX MATCH "^(.+)\.(vs|ps|cs)\.hlsl$" _match ${_file})
        if(NOT _match)
            message(FATAL_ERROR "Shader '${_file}' must be named <name>.<vs|ps|cs>.hlsl")
        endif()
        set(_name ${CMAKE_MATCH_1}.${CMAKE_MATCH_2})
        set(_profile ${CMAKE_MATCH_2}_6_0)

        set(_spv ${F3D_SHADER_BINARY_DIR}/${_name}.spv)
        add_custom_command(OUTPUT ${_spv}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${F3D_SHADER_BINARY_DIR}
            COMMAND ${F3D_DXC} ${F3D_DXC_FLAGS} ${F3D_DXC_SPIRV_FLAGS} -T ${_profile} -Fo ${_spv} ${src}
            DEPENDS ${src} ${_includes}
            COMMENT "DXC (SPIR-V) ${_name}"
            VERBATIM)
        list(APPEND _outputs ${_spv})

        if(WIN32)
            set(_dxil ${F3D_SHADER_BINARY_DIR}/${_name}.dxil)
            add_custom_command(OUTPUT ${_dxil}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${F3D_SHADER_BINARY_DIR}
                COMMAND ${F3D_DXC} ${F3D_DXC_FLAGS} -T ${_profile} -Fo ${_dxil} ${src}
                DEPENDS ${src} ${_includes}
                COMMENT "DXC (DXIL) ${_name}"
                VERBATIM)
            list(APPEND _outputs ${_dxil})
        endif()
    endforeach()
    add_custom_target(${target} ALL DEPENDS ${_outputs} SOURCES ${ARGN})
endfunction()

# Copies the DXC runtime next to an executable: Diligent's D3D12 backend needs
# dxcompiler/dxil to reflect DXIL and to compile shaders at run time.
function(f3d_deploy_dxc_runtime target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Microsoft::DirectXShaderCompiler> $<TARGET_FILE:Microsoft::DXIL>
                $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    endif()
endfunction()
