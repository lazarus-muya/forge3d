// SPDX-License-Identifier: Apache-2.0
// Copies the scene depth, rendered below the output size (screen percentage), to an
// output-sized depth target (nearest), so the overlays drawn at output size (grid,
// debug lines) are depth-tested against the scene.

Texture2D<float> g_SceneDepth;

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

float main(PSInput input) : SV_Depth
{
    uint width, height;
    g_SceneDepth.GetDimensions(width, height);
    float2 uv = input.Ndc * float2(0.5, -0.5) + 0.5;
    uint2 texel = min(uint2(uv * float2(width, height)), uint2(width - 1, height - 1));
    return g_SceneDepth.Load(int3(texel, 0));
}
