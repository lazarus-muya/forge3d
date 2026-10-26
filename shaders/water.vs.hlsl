// SPDX-License-Identifier: Apache-2.0
// Water surfaces (phases.md P7.3): lakes and rivers come in world space; the ocean is
// a grid around the camera at the water level. Gerstner waves displace lakes and the
// ocean (rivers only ripple, in water.ps).
#include "common.hlsli"
#include "view.hlsli"
#include "water.hlsli"

struct VSInput
{
    F3D_LOCATION(0) float3 Position : ATTRIB0; // world (ocean: offset from the grid centre)
    F3D_LOCATION(1) float2 Flow     : ATTRIB1; // rivers: x/z metres per second
};

struct PSInput
{
    float4 Pos      : SV_Position;
    float3 WorldPos : TEXCOORD0;
    float3 Normal   : NORMAL;
    float2 Flow     : TEXCOORD1;
    float  Crest    : TEXCOORD2;
};

PSInput main(VSInput input)
{
    float3 world = input.Position;
    if ((uint)g_WaterParams.w == F3D_WATER_OCEAN)
        world = float3(input.Position.x + g_WaterOcean.x, g_WaterOcean.z, input.Position.z + g_WaterOcean.y);
    float3 offset, normal;
    float crest;
    Gerstner(world.xz, offset, normal, crest);
    // Fade the swell out in the distance, where the grid is coarse.
    float fade = saturate(1.0 - length(world.xz - g_CameraPos.xz) / max(g_WaterWaves.y * 60.0, 200.0));
    world += offset * fade;

    PSInput output;
    output.Pos = mul(g_ViewProj, float4(world, 1.0));
    output.WorldPos = world;
    output.Normal = normalize(lerp(float3(0.0, 1.0, 0.0), normal, fade));
    output.Flow = input.Flow;
    output.Crest = crest * fade;
    return output;
}
