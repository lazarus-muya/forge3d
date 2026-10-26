// SPDX-License-Identifier: Apache-2.0
// Forward pass: mesh-space vertices with a per-object world matrix (shaded by mesh.ps).
#include "common.hlsli"
#include "object.hlsli"
#include "view.hlsli"

struct VSInput
{
    F3D_LOCATION(0) float3 Pos    : ATTRIB0;
    F3D_LOCATION(1) float3 Normal : ATTRIB1;
    F3D_LOCATION(2) float4 Color  : ATTRIB2; // RGBA8 unorm, sRGB-encoded vertex colour
    F3D_LOCATION(3) float2 UV     : ATTRIB3; // second vertex stream (zeros without UVs)
};

struct PSInput
{
    float4 Pos      : SV_Position;
    float3 Normal   : NORMAL;
    float3 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
};

PSInput main(VSInput input)
{
    PSInput output;
    float4 world    = mul(g_World, float4(input.Pos, 1.0));
    output.Pos      = mul(g_ViewProj, world);
    output.Normal   = mul((float3x3)g_NormalMatrix, input.Normal);
    output.Color    = SrgbToLinear(input.Color.rgb);
    output.UV       = input.UV;
    output.WorldPos = world.xyz;
    return output;
}
