// SPDX-License-Identifier: Apache-2.0
// Picking pass: writes the entity id (g_ObjectId.x) to an R32_UINT target.
#include "object.hlsli"

struct PSInput // mesh.vs.hlsl's output
{
    float4 Pos      : SV_Position;
    float3 Normal   : NORMAL;
    float3 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
};

uint main(PSInput input) : SV_Target
{
    return g_ObjectId.x;
}
