// SPDX-License-Identifier: Apache-2.0
// Per-draw constants (f3d::render::raster::ObjectConstants).
#ifndef F3D_OBJECT_HLSLI
#define F3D_OBJECT_HLSLI

cbuffer ObjectConstants
{
    float4x4 g_World;
    float4x4 g_NormalMatrix; // inverse transpose of the world matrix (upper 3x3 used)
    uint4    g_ObjectId;     // x: entity index + 1 (0 = nothing), y: environment slot (0 sky, then probes)
};

#endif // F3D_OBJECT_HLSLI
