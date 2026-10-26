// SPDX-License-Identifier: Apache-2.0
// Forward+ shading shared by mesh.ps and terrain.ps (phases.md P4.2, P4.3, P6.4): the
// environment (SH irradiance + prefiltered specular), cascaded shadows, every
// directional light, and the local lights of the pixel's cluster.
#ifndef F3D_FORWARD_HLSLI
#define F3D_FORWARD_HLSLI

#include "atmosphere.hlsli"
#include "ibl.hlsli"
#include "lighting.hlsli"

StructuredBuffer<GpuLight> g_Lights;
StructuredBuffer<uint>     g_ClusterCounts;
StructuredBuffer<uint>     g_ClusterIndices;

StructuredBuffer<float4> g_EnvSH; // 9 per slot (0: sky, then probes)
TextureCube<float4>      g_EnvSpecular;
SamplerState             g_EnvSpecular_sampler;
Texture2DArray<float>    g_ShadowMap;
SamplerComparisonState   g_ShadowMap_sampler;

// Tangent frame from screen-space derivatives (no precomputed tangents needed).
float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    return float3x3(T * invmax, B * invmax, N);
}

float3 EnvIrradiance(uint slot, float3 n)
{
    float b[9];
    ShBasis(n, b);
    float3 e = 0.0;
    for (uint i = 0; i < 9; ++i)
        e += g_EnvSH[slot * 9 + i].rgb * b[i];
    return max(e, 0.0);
}

float3 ShadeEnvironment(Surface s, float occlusion, uint slot)
{
    float  NoV = max(dot(s.Normal, s.View), 1e-4);
    float3 R   = reflect(-s.View, s.Normal);
    float3 diffuse = s.Diffuse * EnvIrradiance(slot, s.Normal) / F3D_PI;
    float  mip = s.Roughness * max(g_FrameParams.y - 1.0, 0.0);
    float3 prefiltered = g_EnvSpecular.SampleLevel(g_EnvSpecular_sampler, R, mip).rgb / F3D_ENV_SCALE;
    float3 specular = EnvBRDFApprox(s.F0, s.Roughness, NoV) * prefiltered;
    return (diffuse + specular) * occlusion;
}

// Cascaded shadow map lookup with a normal offset and a 3x3 PCF kernel; fades out
// over the last 10% of the shadow distance.
float ShadowFactor(float3 worldPos, float3 N, float viewDepth)
{
    if (g_LightCounts.z == 0 || viewDepth >= g_ShadowSplits.w)
        return 1.0;
    uint cascade = 0;
    [unroll] for (uint c = 0; c < F3D_SHADOW_CASCADES - 1; ++c)
        cascade += viewDepth > g_ShadowSplits[c] ? 1 : 0;
    float3 L = g_DirLightDirection[g_LightCounts.z - 1].xyz;
    float texel = g_ShadowTexel[cascade];
    float NoL = saturate(dot(N, L));
    float3 p = worldPos + N * texel * (1.5 - NoL) + L * texel * 0.5;
    float4 clip = mul(g_ShadowMatrices[cascade], float4(p, 1.0));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
        return 1.0;
    float width, height, slices;
    g_ShadowMap.GetDimensions(width, height, slices);
    float2 step = 1.0 / float2(width, height);
    float lit = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            lit += g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(uv + float2(x, y) * step, cascade),
                                                  saturate(ndc.z));
    lit /= 9.0;
    float fade = saturate((viewDepth - g_ShadowSplits.w * 0.9) / (g_ShadowSplits.w * 0.1));
    return lerp(lit, 1.0, fade);
}

// Everything but emission: the environment from `envSlot` and every light.
// `geometricNormal`: the interpolated vertex normal (shadow offsets).
float3 ShadeForward(Surface s, float occlusion, uint envSlot, float3 geometricNormal, float2 pixel)
{
    float viewDepth = -mul(g_View, float4(s.Position, 1.0)).z;
    float3 color = ShadeEnvironment(s, occlusion, envSlot);
    for (uint d = 0; d < g_LightCounts.y; ++d)
    {
        float3 irradiance = g_DirLightColor[d].rgb;
        if (d + 1 == g_LightCounts.z)
            irradiance *= ShadowFactor(s.Position, geometricNormal, viewDepth);
        color += ShadeLight(s, g_DirLightDirection[d].xyz, irradiance);
    }

    uint  cluster   = ClusterAt(pixel, viewDepth);
    uint  count     = min(g_ClusterCounts[cluster], F3D_MAX_LIGHTS_PER_CLUSTER);
    for (uint i = 0; i < count; ++i)
        color += ShadeLocalLight(s, g_Lights[g_ClusterIndices[cluster * F3D_MAX_LIGHTS_PER_CLUSTER + i]]);
    return color;
}

// The cascades view mode's tint, then pre-exposure.
float4 FinishForward(float3 color, float3 worldPos)
{
    float viewDepth = -mul(g_View, float4(worldPos, 1.0)).z;
    if (ViewMode() == F3D_VIEW_CASCADES && g_LightCounts.z != 0 && viewDepth < g_ShadowSplits.w)
    {
        const float3 tints[4] = {float3(1.0, 0.35, 0.35), float3(0.35, 1.0, 0.35), float3(0.35, 0.5, 1.0),
                                 float3(1.0, 1.0, 0.3)};
        uint cascade = 0;
        [unroll] for (uint c = 0; c < F3D_SHADOW_CASCADES - 1; ++c)
            cascade += viewDepth > g_ShadowSplits[c] ? 1 : 0;
        color *= tints[cascade];
    }
    return float4(min(color * FrameExposure(), 60000.0), 1.0);
}

#endif // F3D_FORWARD_HLSLI
