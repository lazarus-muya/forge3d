// SPDX-License-Identifier: Apache-2.0
// Fog composite (fog.hlsli): the volumetric fog up to its range, then the analytic
// height fog to the surface (or the sky distance), over the pre-exposed scene colour.
// Premultiplied output: scene * (1 - a) + rgb.
#include "fog.hlsli"

Texture2D<float>  g_SceneDepth;
Texture3D<float4> g_FogIntegrated;
SamplerState      g_FogIntegrated_sampler;

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

// In-scattered light and transmittance from the camera to `viewDepth` (<= the volume's range).
float4 SampleVolume(float2 uv, float viewDepth)
{
    float slices = g_FogGrid.z;
    float w = FroxelW(viewDepth) * slices; // in slices
    if (w < 1.0) // inside the first slice: towards nothing at the camera
        return lerp(float4(0.0, 0.0, 0.0, 1.0),
                    g_FogIntegrated.SampleLevel(g_FogIntegrated_sampler, float3(uv, 0.5 / slices), 0.0), w);
    // Slice z holds the integral to its far side, at w = z + 1.
    return g_FogIntegrated.SampleLevel(g_FogIntegrated_sampler, float3(uv, (w - 0.5) / slices), 0.0);
}

float4 main(PSInput input) : SV_Target
{
    float depth = g_SceneDepth.Load(int3(input.Pos.xy, 0));
    bool sky = depth >= 1.0;
    if (sky && g_FogCounts.y == 0)
        discard; // the UI background, not scene
    float2 uv = input.Pos.xy * g_Viewport.zw;
    FogRay ray = FogRayAt(uv);
    float3 dir = normalize(ray.Far - ray.Near);
    float lengthPerDepth = FogRayLengthPerDepth(ray);
    float distance;
    if (sky)
    {
        distance = g_FogRange.y;
    }
    else
    {
        float4 p = mul(g_InvViewProj, float4(input.Ndc, depth, 1.0));
        distance = length(p.xyz / p.w - ray.Near);
    }

    float4 volume = float4(0.0, 0.0, 0.0, 1.0);
    float start = g_FogLight.z;
    if (g_FogRange.x > 0.0)
    {
        volume = SampleVolume(uv, min(distance / lengthPerDepth, g_FogRange.x));
        start = max(start, g_FogRange.x * lengthPerDepth);
    }
    float T = exp(-HeightFogOpticalDepth(ray.Near, dir, start, distance));
    T = max(T, 1.0 - g_FogLight.w);
    float3 inscatter = (1.0 - T) * g_FogAlbedo.rgb * (FogAmbient() + FogDirectional(-dir, 1.0)) * FrameExposure();
    return float4(volume.rgb + volume.a * inscatter, 1.0 - volume.a * T);
}
