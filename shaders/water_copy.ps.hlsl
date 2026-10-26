// SPDX-License-Identifier: Apache-2.0
// Copies the scene colour and depth before the water is drawn over them: the water
// refracts the copy and measures its depth against it (phases.md P7.3).

Texture2D        g_SceneColor;
Texture2D<float> g_SceneDepth;

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

struct PSOutput
{
    float4 Color : SV_Target0;
    float  Depth : SV_Target1;
};

PSOutput main(PSInput input)
{
    int3 p = int3(input.Pos.xy, 0);
    PSOutput output;
    output.Color = g_SceneColor.Load(p);
    output.Depth = g_SceneDepth.Load(p);
    return output;
}
