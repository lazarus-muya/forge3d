// SPDX-License-Identifier: Apache-2.0
#include "common.hlsli"
#include "view.hlsli"

struct VSInput
{
    F3D_LOCATION(0) float3 Pos   : ATTRIB0;
    F3D_LOCATION(1) float4 Color : ATTRIB1; // RGBA8 unorm, sRGB-encoded
};

struct PSInput
{
    float4 Pos   : SV_Position;
    float4 Color : COLOR0;
};

PSInput main(VSInput input)
{
    PSInput output;
    output.Pos   = mul(g_ViewProj, float4(input.Pos, 1.0));
    output.Color = float4(SrgbToLinear(input.Color.rgb), input.Color.a);
    return output;
}
