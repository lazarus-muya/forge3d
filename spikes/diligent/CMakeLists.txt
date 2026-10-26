# SPDX-License-Identifier: Apache-2.0
# P0.3: Diligent + SDL3 + ImGui spike. See docs/spikes/diligent.md.
include(F3DDiligent)
find_package(SDL3 CONFIG REQUIRED)
find_package(Stb REQUIRED)

add_executable(forge3d-spike-diligent main.cpp)
target_include_directories(forge3d-spike-diligent SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(forge3d-spike-diligent PRIVATE f3d_options f3d_core Diligent::Engine Diligent::ImGui SDL3::SDL3)

foreach(rhi IN ITEMS d3d12 vulkan)
    if((rhi STREQUAL "d3d12" AND NOT WIN32))
        continue()
    endif()
    add_test(NAME integration.spike.diligent.${rhi}
             COMMAND forge3d-spike-diligent --rhi=${rhi} --frames 120 --hidden)
    set_tests_properties(integration.spike.diligent.${rhi} PROPERTIES LABELS "integration;spike" TIMEOUT 120)
endforeach()
