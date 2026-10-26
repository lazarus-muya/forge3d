// SPDX-License-Identifier: Apache-2.0
// RHI integration test: vertex buffer + uniform buffer + interpolated colour.
#include "common.hlsli"

cbuffer Constants
{
    float4 g_Tint;
};

struct VSInput
{
    F3D_LOCATION(0) float2 Pos   : ATTRIB0;
    F3D_LOCATION(1) float3 Color : ATTRIB1;
};

struct PSInput
{
    float4 Pos   : SV_Position;
    float3 Color : COLOR0;
};

PSInput main(VSInput input)
{
    PSInput output;
    output.Pos   = float4(input.Pos, 0.0, 1.0);
    output.Color = input.Color * g_Tint.rgb;
    return output;
}
