// SPDX-License-Identifier: Apache-2.0
// One triangle covering the screen (draw 3 vertices, no vertex buffer).
// Used by the ground grid (ray from Ndc) and full-screen passes (Pos).

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

PSInput main(uint id : SV_VertexID)
{
    // (0,0) (2,0) (0,2) -> a triangle covering the screen.
    float2 uv = float2((id << 1) & 2, id & 2);
    PSInput output;
    output.Ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    output.Pos = float4(output.Ndc, 0.0, 1.0);
    return output;
}
