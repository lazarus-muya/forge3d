// SPDX-License-Identifier: Apache-2.0
// Image-based lighting helpers (phases.md P4.3): cube-map texel directions, the
// filter pass constants and L2 spherical harmonics.
#ifndef F3D_IBL_HLSLI
#define F3D_IBL_HLSLI

// f3d::render::raster::FilterConstants.
cbuffer FilterConstants
{
    float4 g_FilterParams; // x: roughness, y: output face size, z: source face size (mip 0), w: source mip count
    uint4  g_FilterSlot;   // x: SH slot (9 float4 each)
};

static const float F3D_IBL_PI = 3.14159265;

// World direction of texel (x, y) of cube face `face` (D3D order +X -X +Y -Y +Z -Z).
float3 CubeDirection(uint face, float2 texel, float size)
{
    float2 uv = (texel + 0.5) / size * 2.0 - 1.0;
    float3 d;
    switch (face)
    {
    case 0: d = float3(1.0, -uv.y, -uv.x); break;
    case 1: d = float3(-1.0, -uv.y, uv.x); break;
    case 2: d = float3(uv.x, 1.0, uv.y); break;
    case 3: d = float3(uv.x, -1.0, -uv.y); break;
    case 4: d = float3(uv.x, -uv.y, 1.0); break;
    default: d = float3(-uv.x, -uv.y, -1.0); break;
    }
    return normalize(d);
}

// Solid angle of a cube texel (relative to the face size), for SH projection.
float CubeTexelSolidAngle(float2 texel, float size)
{
    float2 uv = (texel + 0.5) / size * 2.0 - 1.0;
    float t = 1.0 + dot(uv, uv);
    return 4.0 / (size * size * t * sqrt(t));
}

// The 9 real SH basis functions (bands 0-2).
void ShBasis(float3 n, out float b[9])
{
    b[0] = 0.282095;
    b[1] = 0.488603 * n.y;
    b[2] = 0.488603 * n.z;
    b[3] = 0.488603 * n.x;
    b[4] = 1.092548 * n.x * n.y;
    b[5] = 1.092548 * n.y * n.z;
    b[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    b[7] = 1.092548 * n.x * n.z;
    b[8] = 0.546274 * (n.x * n.x - n.y * n.y);
}

#endif // F3D_IBL_HLSLI
