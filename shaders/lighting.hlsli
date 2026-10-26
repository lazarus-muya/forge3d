// SPDX-License-Identifier: Apache-2.0
// Lights, clusters and the metal/rough BRDF (phases.md P4.2). The cluster math
// matches f3d::render::raster (lights.hpp); tests compare the two.
#ifndef F3D_LIGHTING_HLSLI
#define F3D_LIGHTING_HLSLI

#include "view.hlsli"

#define F3D_CLUSTER_TILES_X 16
#define F3D_CLUSTER_TILES_Y 9
#define F3D_CLUSTER_SLICES 24
#define F3D_CLUSTER_COUNT (F3D_CLUSTER_TILES_X * F3D_CLUSTER_TILES_Y * F3D_CLUSTER_SLICES)
#define F3D_MAX_LIGHTS_PER_CLUSTER 64

static const float F3D_PI = 3.14159265;

struct GpuLight
{
    float3 Position;  // world
    float  Range;
    float3 Color;     // colour x intensity
    uint   Type;      // 0 point, 1 spot
    float3 Direction; // spot axis, away from the light
    float  SpotScale;
    float  SpotOffset;
    float  Volumetric; // scattering scale in volumetric fog
    float2 Pad;
};

uint ClusterSlice(float viewDepth)
{
    float slice = floor(log(max(viewDepth, 1e-6)) * g_ClusterParams.z + g_ClusterParams.w);
    return (uint)clamp(slice, 0.0, F3D_CLUSTER_SLICES - 1.0);
}

uint ClusterIndex(uint x, uint y, uint z)
{
    return (z * F3D_CLUSTER_TILES_Y + y) * F3D_CLUSTER_TILES_X + x;
}

// The cluster of a pixel (SV_Position.xy) at a view-space depth.
uint ClusterAt(float2 pixel, float viewDepth)
{
    uint x = min((uint)max(pixel.x, 0.0) / (uint)g_ClusterParams.x, F3D_CLUSTER_TILES_X - 1);
    uint y = min((uint)max(pixel.y, 0.0) / (uint)g_ClusterParams.y, F3D_CLUSTER_TILES_Y - 1);
    return ClusterIndex(x, y, ClusterSlice(viewDepth));
}

// --- BRDF ---------------------------------------------------------------------------------

float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float d  = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (F3D_PI * d * d);
}

// Height-correlated Smith visibility (includes the 1 / (4 NoL NoV) term).
float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float v  = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float l  = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-5);
}

float3 F_Schlick(float VoH, float3 f0)
{
    float f = pow(1.0 - VoH, 5.0);
    return f0 + (1.0 - f0) * f;
}

struct Surface
{
    float3 Position; // world
    float3 Normal;   // unit, world
    float3 View;     // unit, towards the eye
    float3 Diffuse;  // albedo * (1 - metallic)
    float3 F0;       // specular reflectance at normal incidence
    float  Roughness; // perceptual
};

// Radiance reflected towards the eye for light arriving from L with `irradiance` (normal incidence).
float3 ShadeLight(Surface s, float3 L, float3 irradiance)
{
    float NoL = saturate(dot(s.Normal, L));
    if (NoL <= 0.0)
        return 0.0;
    float3 H   = normalize(s.View + L);
    float  NoV = max(dot(s.Normal, s.View), 1e-4);
    float  NoH = saturate(dot(s.Normal, H));
    float  VoH = saturate(dot(s.View, H));
    float  a   = max(s.Roughness * s.Roughness, 0.002);
    float3 F   = F_Schlick(VoH, s.F0);
    float3 specular = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;
    float3 diffuse  = s.Diffuse * (1.0 / F3D_PI);
    return (diffuse + specular) * irradiance * NoL;
}

// Windowed inverse-square falloff: reaches zero at the light's range.
float DistanceAttenuation(float distanceSq, float range)
{
    float ratio  = distanceSq / (range * range);
    float window = saturate(1.0 - ratio * ratio);
    return window * window / max(distanceSq, 1e-4);
}

float3 ShadeLocalLight(Surface s, GpuLight light)
{
    float3 toLight = light.Position - s.Position;
    float  d2      = dot(toLight, toLight);
    if (d2 >= light.Range * light.Range)
        return 0.0;
    float3 L   = toLight * rsqrt(max(d2, 1e-8));
    float  att = DistanceAttenuation(d2, light.Range);
    if (light.Type == 1)
    {
        float cd = dot(light.Direction, -L);
        float spot = saturate(cd * light.SpotScale + light.SpotOffset);
        att *= spot * spot;
    }
    return ShadeLight(s, L, light.Color * att);
}

// Analytic approximation of the split-sum environment BRDF (Karis, "Physically Based Shading on Mobile").
float3 EnvBRDFApprox(float3 f0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r   = roughness * c0 + c1;
    float  a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 ab  = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

#endif // F3D_LIGHTING_HLSLI
