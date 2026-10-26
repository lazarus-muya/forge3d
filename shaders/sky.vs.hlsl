// SPDX-License-Identifier: Apache-2.0
// One triangle covering the screen at the far plane (depth 1): the sky pass draws
// only where no geometry was (depth test LessEqual against a depth cleared to 1).

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

PSInput main(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    PSInput output;
    output.Ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    output.Pos = float4(output.Ndc, 1.0, 1.0);
    return output;
}
