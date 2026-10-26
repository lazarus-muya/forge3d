// SPDX-License-Identifier: Apache-2.0
// Volumetric fog, step 2 (fog.hlsli): front-to-back integration of every froxel
// column. Slice z of the output holds the light scattered towards the eye (rgb,
// pre-exposed) and the transmittance (a) from the camera to the far side of slice z.
// Each slice's medium is constant, integrated exactly (energy conserving, Hillaire 2015).
#include "fog.hlsli"

Texture3D<float4>   g_FogScatter;
RWTexture3D<float4> g_FogIntegrated;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint3 size = (uint3)g_FogGrid.xyz;
    if (any(id.xy >= size.xy))
        return;
    float lengthPerDepth = FogRayLengthPerDepth(FogRayAt((float2(id.xy) + 0.5) / float2(size.xy)));
    float3 L = 0.0;
    float T = 1.0;
    float previous = 0.0;
    for (uint z = 0; z < size.z; ++z)
    {
        float next = FroxelDepth((z + 1.0) / size.z);
        float ds = (next - previous) * lengthPerDepth;
        previous = next;
        float4 cell = g_FogScatter[uint3(id.xy, z)];
        float extinction = max(cell.a, 1e-7);
        float stepT = exp(-extinction * ds);
        L += T * (cell.rgb - cell.rgb * stepT) / extinction;
        T *= stepT;
        g_FogIntegrated[uint3(id.xy, z)] = float4(L, T);
    }
}
