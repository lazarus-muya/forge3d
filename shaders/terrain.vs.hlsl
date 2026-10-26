// SPDX-License-Identifier: Apache-2.0
// Terrain (phases.md P6.1): CDLOD (Strugar 2009). One grid mesh, instanced per
// selected quadtree node; vertices morph towards the next coarser LOD's grid as they
// approach their LOD's range, so neighbouring LODs meet without cracks. Heights come
// from the height texture; normals are computed here from it. The output matches
// mesh.vs; terrain.ps blends the material layers and pick.ps writes the ID buffer.
#include "common.hlsli"
#include "terrain.hlsli"
#include "view.hlsli"

struct VSInput
{
    F3D_LOCATION(0) float2 Grid : ATTRIB0; // 0 .. 32
};

struct PSInput
{
    float4 Pos      : SV_Position;
    float3 Normal   : NORMAL;
    float3 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
};

float DecodeHeight(float raw)
{
    return g_TerrainParams.w > 0.0 ? g_TerrainParams.z + raw * g_TerrainParams.w : raw;
}

// Bilinear height at a (fractional) sample position.
float HeightAt(float2 samplePos)
{
    float n = g_TerrainParams.x;
    float2 uv = (clamp(samplePos, 0.0, n - 1.0) + 0.5) / n;
    return DecodeHeight(g_TerrainHeight.SampleLevel(g_TerrainHeight_sampler, uv, 0));
}

float3 LocalPosition(float2 samplePos)
{
    float2 xz = clamp(samplePos, 0.0, g_TerrainParams.x - 1.0) * g_TerrainOrigin.w - g_TerrainParams.y * 0.5;
    return float3(xz.x, HeightAt(samplePos), xz.y);
}

PSInput main(VSInput input, uint instance : SV_InstanceID)
{
    uint4 node = g_TerrainNodes[instance + (uint)g_TerrainDraw.x];
    float step = (float)(1u << node.z);
    float2 grid = input.Grid;
    float2 samplePos = float2(node.xy) + grid * step;

    // Morph odd grid lines onto the coarser LOD's as the vertex nears its range.
    float3 approx = LocalPosition(samplePos);
    float distance = length(approx - g_TerrainCamera.xyz);
    float4 morph = g_TerrainMorph[min(node.z, F3D_TERRAIN_MAX_LODS - 1)];
    float k = saturate((distance - morph.x) * morph.y);
    float2 odd = frac(grid * 0.5) * 2.0;
    grid -= odd * k;
    samplePos = float2(node.xy) + grid * step;

    float3 local = LocalPosition(samplePos);
    // Normal from the heights around (one sample apart: full detail at every LOD).
    float hl = HeightAt(samplePos - float2(1.0, 0.0));
    float hr = HeightAt(samplePos + float2(1.0, 0.0));
    float hd = HeightAt(samplePos - float2(0.0, 1.0));
    float hu = HeightAt(samplePos + float2(0.0, 1.0));
    float3 normal = normalize(float3(hl - hr, 2.0 * g_TerrainOrigin.w, hd - hu));

    float3 world = local + g_TerrainOrigin.xyz;
    PSInput output;
    output.Pos = mul(g_TerrainViewProj, float4(world, 1.0));
    output.Normal = normal;
    output.Color = 1.0; // terrain.ps blends the material layers
    output.UV = local.xz * 0.25;
    output.WorldPos = world;
    return output;
}
