// SPDX-License-Identifier: Apache-2.0
// Residency level selection (phases.md P3.5).
#include <doctest/doctest.h>

#include "f3d/render/raster/residency.hpp"

using f3d::render::raster::ResidencyManager;

TEST_CASE("residency: LOD for an on-screen radius") {
    CHECK(ResidencyManager::lod_for(500.0f, 4) == 0);
    CHECK(ResidencyManager::lod_for(200.0f, 4) == 0);
    CHECK(ResidencyManager::lod_for(150.0f, 4) == 1);
    CHECK(ResidencyManager::lod_for(80.0f, 4) == 2);
    CHECK(ResidencyManager::lod_for(30.0f, 4) == 3);
    CHECK(ResidencyManager::lod_for(1.0f, 4) == 3); // clamped to the coarsest
    CHECK(ResidencyManager::lod_for(0.0f, 2) == 1);
    CHECK(ResidencyManager::lod_for(5.0f, 1) == 0); // a single LOD
}

TEST_CASE("residency: top mip for a texture over an on-screen radius") {
    // 1024 texels over 512 px (diameter 1024): full resolution.
    CHECK(ResidencyManager::mip_for(512.0f, 1024, 11) == 0);
    CHECK(ResidencyManager::mip_for(1000.0f, 1024, 11) == 0);
    CHECK(ResidencyManager::mip_for(256.0f, 1024, 11) == 1);
    CHECK(ResidencyManager::mip_for(64.0f, 1024, 11) == 3);
    CHECK(ResidencyManager::mip_for(0.1f, 1024, 11) == 10); // clamped to 1x1
    CHECK(ResidencyManager::mip_for(10.0f, 1024, 1) == 0);  // no mips
}
