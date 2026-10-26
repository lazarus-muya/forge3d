# SPDX-License-Identifier: Apache-2.0
# The only module that may use Diligent (cmake/F3DLayering.cmake).
include(F3DDiligent)
include(F3DShaders)

f3d_add_module(f3d_render_rhi
    SOURCES
        src/command_context.cpp
        src/device.cpp
        src/imgui_renderer.cpp
        src/resources.cpp
        src/shader_library.cpp
        src/swapchain.cpp
        src/types.cpp
    DEPS f3d_core
    PRIVATE_LIBS Diligent::Engine Diligent::ImGui)

if(TARGET volk::volk)
    # Vulkan memory query in Device::memory_usage (volk's function pointers are loaded by Diligent).
    target_link_libraries(f3d_render_rhi PRIVATE volk::volk Vulkan::Headers)
endif()
if(WIN32)
    target_link_libraries(f3d_render_rhi PRIVATE dxgi)
endif()

# Build-time shader configuration, shared with run-time hot reload.
function(_f3d_cpp_string_list out)
    set(_items)
    foreach(arg IN LISTS ARGN)
        string(REPLACE "\\" "/" arg "${arg}")
        list(APPEND _items "\"${arg}\"")
    endforeach()
    list(JOIN _items ", " _joined)
    set(${out} "${_joined}" PARENT_SCOPE)
endfunction()
_f3d_cpp_string_list(F3D_DXC_FLAG_LIST ${F3D_DXC_FLAGS})
_f3d_cpp_string_list(F3D_DXC_SPIRV_FLAG_LIST ${F3D_DXC_SPIRV_FLAGS})
configure_file(src/shader_config.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/f3d_shader_config.hpp @ONLY)
target_include_directories(f3d_render_rhi PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(f3d_render_rhi PRIVATE $<$<CONFIG:Debug>:F3D_SHADER_HOT_RELOAD=1>)
