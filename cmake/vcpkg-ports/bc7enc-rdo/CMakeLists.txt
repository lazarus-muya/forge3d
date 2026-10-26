# SPDX-License-Identifier: Apache-2.0
# Build script for the bc7enc-rdo overlay port: the encoder/decoder library only.
cmake_minimum_required(VERSION 3.20)
project(bc7enc_rdo CXX)

add_library(bc7enc_rdo bc7enc.cpp rgbcx.cpp bc7decomp.cpp)
target_compile_features(bc7enc_rdo PUBLIC cxx_std_11)
target_include_directories(bc7enc_rdo PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                             $<INSTALL_INTERFACE:include/bc7enc_rdo>)

install(TARGETS bc7enc_rdo EXPORT bc7enc_rdo-targets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(FILES bc7enc.h rgbcx.h bc7decomp.h rgbcx_table4.h rgbcx_table4_small.h DESTINATION include/bc7enc_rdo)
install(EXPORT bc7enc_rdo-targets NAMESPACE bc7enc_rdo:: FILE bc7enc_rdo-config.cmake DESTINATION share/bc7enc_rdo)
