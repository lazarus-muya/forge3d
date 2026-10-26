// SPDX-License-Identifier: Apache-2.0

#include "view.hlsli"

struct PSInput
{
    float4 Pos   : SV_Position;
    float4 Color : COLOR0;
};

float4 main(PSInput input) : SV_Target
{
    return float4(LinearToSrgbOverlay(input.Color.rgb), input.Color.a); // drawn after tonemapping
}
