# SPDX-License-Identifier: Apache-2.0
find_package(spdlog CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Taskflow CONFIG REQUIRED)

set(F3D_CORE_PUBLIC_LIBS spdlog::spdlog glm::glm EnTT::EnTT nlohmann_json::nlohmann_json)
if(F3D_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    list(APPEND F3D_CORE_PUBLIC_LIBS Tracy::TracyClient)
endif()

f3d_add_module(f3d_core
    SOURCES
        src/assert.cpp
        src/debug_draw.cpp
        src/file_watcher.cpp
        src/guid.cpp
        src/image.cpp
        src/image_compare.cpp
        src/jobs.cpp
        src/log.cpp
        src/reflect.cpp
        src/snap.cpp
        src/stats.cpp
        src/system.cpp
        src/time.cpp
        src/vfs.cpp
        src/version.cpp
    PUBLIC_LIBS ${F3D_CORE_PUBLIC_LIBS})

target_compile_definitions(f3d_core PUBLIC
    F3D_TRACY_ENABLED=$<BOOL:${F3D_TRACY}>
    # Must be identical in every translation unit that uses GLM (f3d/core/math.hpp).
    GLM_FORCE_INTRINSICS GLM_ENABLE_EXPERIMENTAL)
target_include_directories(f3d_core SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(f3d_core PRIVATE Taskflow::Taskflow)
target_compile_definitions(f3d_core PRIVATE F3D_VERSION="${PROJECT_VERSION}")
