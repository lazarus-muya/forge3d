# SPDX-License-Identifier: Apache-2.0
# Build script for the ufbx overlay port (upstream ships no CMake build).
cmake_minimum_required(VERSION 3.20)
project(ufbx C)

add_library(ufbx ufbx.c)
target_include_directories(ufbx PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> $<INSTALL_INTERFACE:include>)
if(NOT MSVC)
    target_link_libraries(ufbx PRIVATE m)
endif()

install(TARGETS ufbx EXPORT ufbx-targets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES ufbx.h DESTINATION include)
install(EXPORT ufbx-targets NAMESPACE ufbx:: FILE ufbx-config.cmake DESTINATION share/ufbx)
