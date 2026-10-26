# SPDX-License-Identifier: Apache-2.0
# Build script for the xatlas overlay port (upstream builds with premake).
cmake_minimum_required(VERSION 3.20)
project(xatlas CXX)

find_package(Threads REQUIRED)
add_library(xatlas source/xatlas/xatlas.cpp)
target_compile_features(xatlas PUBLIC cxx_std_11)
target_include_directories(xatlas PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/source/xatlas>
                                         $<INSTALL_INTERFACE:include>)
target_link_libraries(xatlas PUBLIC Threads::Threads)

include(CMakePackageConfigHelpers)
install(TARGETS xatlas EXPORT xatlas-targets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES source/xatlas/xatlas.h DESTINATION include)
install(EXPORT xatlas-targets NAMESPACE xatlas:: FILE xatlas-targets.cmake DESTINATION share/xatlas)
configure_package_config_file(xatlas-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/xatlas-config.cmake
                              INSTALL_DESTINATION share/xatlas)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xatlas-config.cmake DESTINATION share/xatlas)
