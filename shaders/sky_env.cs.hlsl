// SPDX-License-Identifier: Apache-2.0
// Environment cube of the sky (phases.md P4.3): the sky-view LUT (physical sky) or
// the hemisphere colours (no Sky entity), without the sun disc (the sun lights the
// scene directly). Radiance times F3D_ENV_SCALE.
#include "atmosphere.hlsli"
#include "ibl.hlsli"

Texture2D<float4> g_SkyViewLut;
SamplerState      g_SkyViewLut_sampler;
RWTexture2DArray<float4> g_OutCube;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float size = g_FilterParams.y;
    if (id.x >= (uint)size || id.y >= (uint)size)
        return;
    float3 dir = CubeDirection(id.z, float2(id.xy), size);
    float3 L;
    if (g_GroundAlbedo.w > 0.5)
        L = g_SkyViewLut.SampleLevel(g_SkyViewLut_sampler, SkyViewUv(dir, g_SkySunDirection.xyz), 0).rgb;
    else
        L = lerp(g_HemiGround.rgb, g_HemiSky.rgb, saturate(dir.y * 0.5 + 0.5)) * F3D_ENV_SCALE;
    g_OutCube[id] = float4(L, 1.0);
}
