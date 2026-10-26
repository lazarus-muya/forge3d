// SPDX-License-Identifier: Apache-2.0
// Shadow-map caster pass (phases.md P4.3): depth only, one cascade per draw set.
#include "common.hlsli"
#include "object.hlsli"

cbuffer ShadowPassConstants
{
    float4x4 g_LightViewProj;
};

struct VSInput
{
    F3D_LOCATION(0) float3 Pos    : ATTRIB0;
    F3D_LOCATION(1) float3 Normal : ATTRIB1;
    F3D_LOCATION(2) float4 Color  : ATTRIB2;
    F3D_LOCATION(3) float2 UV     : ATTRIB3;
};

float4 main(VSInput input) : SV_Position
{
    return mul(g_LightViewProj, mul(g_World, float4(input.Pos, 1.0)));
}
