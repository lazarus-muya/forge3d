// SPDX-License-Identifier: Apache-2.0
// Volumetric fog, step 1 (fog.hlsli): the medium and its lighting in every froxel.
// Writes the light scattered towards the eye per metre (pre-exposed, rgb) and the
// extinction (a), blended with last frame's result reprojected into this view; the
// sample depth in each cell is jittered per frame, so the history converges to the
// cell's average instead of showing the slices.
#include "fog.hlsli"

Texture3D<float4>   g_FogHistory;
SamplerState        g_FogHistory_sampler;
RWTexture3D<float4> g_FogOut;

// Sun shadow at a point in the medium: one comparison sample (the history filters it).
float VolumeShadow(float3 p, float viewDepth)
{
    if (g_LightCounts.z == 0 || viewDepth >= g_ShadowSplits.w)
        return 1.0;
    uint cascade = 0;
    [unroll] for (uint c = 0; c < F3D_SHADOW_CASCADES - 1; ++c)
        cascade += viewDepth > g_ShadowSplits[c] ? 1 : 0;
    float4 clip = mul(g_ShadowMatrices[cascade], float4(p, 1.0));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
        return 1.0;
    return g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(uv, cascade), saturate(ndc.z));
}

float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint3 size = (uint3)g_FogGrid.xyz;
    if (any(id >= size))
        return;
    float2 uv = (float2(id.xy) + 0.5 + g_FogJitter.xy) / float2(size.xy);
    FogRay ray = FogRayAt(uv);
    float jitter = frac(g_FogGrid.w + InterleavedGradientNoise(float2(id.xy) + float(id.z) * float2(5.0, 11.0)));
    float depth = FroxelDepth((id.z + jitter) / size.z);
    float3 p = FogRayPoint(ray, depth);
    float3 V = normalize(ray.Near - ray.Far);

    float distance = depth * FogRayLengthPerDepth(ray);
    float extinction = distance >= g_FogLight.z ? HeightFogExtinction(p) * g_FogRange.z : 0.0;
    float3 scattering = extinction * g_FogAlbedo.rgb;
    float3 emission = 0.0;
    AddFogVolumes(p, extinction, scattering, emission);

    float3 light = FogAmbient() + FogDirectional(V, VolumeShadow(p, depth));
    if (any(scattering > 0.0))
    {
        // Local lights of the cluster this point falls in.
        uint cluster = ClusterAt(uv * g_Viewport.xy, depth);
        uint count = min(g_ClusterCounts[cluster], F3D_MAX_LIGHTS_PER_CLUSTER);
        for (uint i = 0; i < count; ++i)
        {
            GpuLight l = g_Lights[g_ClusterIndices[cluster * F3D_MAX_LIGHTS_PER_CLUSTER + i]];
            float3 toLight = l.Position - p;
            float d2 = dot(toLight, toLight);
            if (d2 >= l.Range * l.Range || l.Volumetric <= 0.0)
                continue;
            float3 L = toLight * rsqrt(max(d2, 1e-8));
            float att = DistanceAttenuation(d2, l.Range);
            if (l.Type == 1)
            {
                float spot = saturate(dot(l.Direction, -L) * l.SpotScale + l.SpotOffset);
                att *= spot * spot;
            }
            light += l.Color * att * HenyeyGreenstein(dot(-L, V), g_FogAlbedo.w) * l.Volumetric;
        }
    }
    float4 current = float4((scattering * light + emission) * FrameExposure(), extinction);

    // Temporal accumulation: the cell centre in last frame's volume.
    if (g_FogRange.w > 0.0)
    {
        float3 centre = FogRayPoint(FogRayAt((float2(id.xy) + 0.5) / float2(size.xy)), FroxelDepth((id.z + 0.5) / size.z));
        float4 clip = mul(g_FogPrevViewProj, float4(centre, 1.0));
        float previousDepth = -dot(g_FogPrevView, float4(centre, 1.0));
        if (clip.w > 0.0 && previousDepth >= 0.0 && previousDepth <= g_FogRange.x)
        {
            float2 previousUv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
            if (all(previousUv >= 0.0) && all(previousUv <= 1.0))
            {
                float4 history = g_FogHistory.SampleLevel(g_FogHistory_sampler,
                                                          float3(previousUv, FroxelW(previousDepth)), 0.0);
                current = lerp(current, history, g_FogRange.w);
            }
        }
    }
    g_FogOut[id] = current;
}
