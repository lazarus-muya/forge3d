// SPDX-License-Identifier: Apache-2.0

struct PSInput
{
    float4 Pos   : SV_Position;
    float3 Color : COLOR0;
};

float4 main(PSInput input) : SV_Target
{
    return float4(input.Color, 1.0);
}
