// SPDX-License-Identifier: Apache-2.0
// Height fog, local fog volumes and volumetric fog (f3d::render::raster::FogConstants,
// fog.hpp). The fog is a participating medium: extinction sigma_t (1/m) and
// scattering sigma_s = albedo * sigma_t, lit by the directional lights (Henyey-
// Greenstein phase), the local lights and the sky (isotropic, its average radiance).
//
// Near the camera (volumetric fog) the medium is evaluated in a froxel volume: cells
// aligned with the view, slices spaced quadratically in view depth out to
// g_FogRange.x. fog_inject.cs lights each cell (sun shadows included), fog_integrate.cs
// accumulates the in-scattered light and transmittance front to back, and fog_apply.ps
// composites both over the scene. Beyond the volume the height fog is integrated
// analytically (the CPU reference is height_fog_optical_depth in fog.cpp).
#ifndef F3D_FOG_HLSLI
#define F3D_FOG_HLSLI

#include "forward.hlsli"

#define F3D_MAX_FOG_VOLUMES 16

struct FogVolumeGpu
{
    float4x4 WorldToBox; // world -> box space; the box is [-0.5, 0.5]^3
    float4   Medium;     // rgb: albedo, a: extinction at the bottom of the box (1/m)
    float4   Shape;      // x: height falloff (1/m), y: box height (m), z: edge fade (0-1)
    float4   Emissive;   // rgb: radiance emitted per metre
};

cbuffer FogConstants
{
    float4   g_FogHeight;     // x: extinction at the base height (1/m), y: height falloff (1/m), z: base height, w: 1 = height fog
    float4   g_FogAlbedo;     // rgb: albedo, w: anisotropy g
    float4   g_FogLight;      // x: sky lighting scale, y: sun lighting scale, z: start distance (m), w: max opacity
    float4   g_FogRange;      // x: volumetric depth (m, 0: no volume), y: sky distance (m), z: volumetric density scale, w: history weight
    float4   g_FogGrid;       // xyz: froxel grid size, w: depth jitter of this frame (0-1)
    float4   g_FogJitter;     // xy: sample offset in the cell this frame (cells, -0.5 to 0.5)
    float4   g_FogDirScatter; // per directional light: volumetric scattering scale
    float4   g_FogPrevView;   // row 2 of last frame's view matrix: view depth = -dot(row, float4(p, 1))
    float4x4 g_FogPrevViewProj;
    uint4    g_FogCounts;     // x: fog volumes, y: 1 = the background is sky (fogged), 0 = UI colour (left alone)
    FogVolumeGpu g_FogVolumes[F3D_MAX_FOG_VOLUMES];
};

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float d  = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * F3D_PI * d * sqrt(d));
}

float FogViewDepth(float3 p)
{
    return -mul(g_View, float4(p, 1.0)).z;
}

// The view ray through a screen position (uv: 0-1, y down): its points on the near and
// far planes and their view depths. Works for both projections.
struct FogRay
{
    float3 Near;
    float3 Far;
    float  NearDepth;
    float  FarDepth;
};

FogRay FogRayAt(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 n = mul(g_InvViewProj, float4(ndc, 0.0, 1.0));
    float4 f = mul(g_InvViewProj, float4(ndc, 1.0, 1.0));
    FogRay r;
    r.Near = n.xyz / n.w;
    r.Far = f.xyz / f.w;
    r.NearDepth = FogViewDepth(r.Near);
    r.FarDepth = FogViewDepth(r.Far);
    return r;
}

float3 FogRayPoint(FogRay r, float viewDepth)
{
    return lerp(r.Near, r.Far, (viewDepth - r.NearDepth) / max(r.FarDepth - r.NearDepth, 1e-6));
}

// Metres along the ray per metre of view depth.
float FogRayLengthPerDepth(FogRay r)
{
    return length(r.Far - r.Near) / max(r.FarDepth - r.NearDepth, 1e-6);
}

// Froxel slices: view depth = range * w^2 for w in [0, 1].
float FroxelDepth(float w)
{
    return g_FogRange.x * w * w;
}

float FroxelW(float viewDepth)
{
    return sqrt(saturate(viewDepth / max(g_FogRange.x, 1e-3)));
}

float HeightFogExtinction(float3 p)
{
    if (g_FogHeight.w < 0.5)
        return 0.0;
    return g_FogHeight.x * exp(clamp(-g_FogHeight.y * (p.y - g_FogHeight.z), -80.0, 40.0));
}

// Optical depth of the height fog along origin + dir * s for s in [s0, s1] (dir unit).
float HeightFogOpticalDepth(float3 origin, float3 dir, float s0, float s1)
{
    if (g_FogHeight.w < 0.5 || s1 <= s0)
        return 0.0;
    float k = g_FogHeight.y;
    float base = g_FogHeight.x * exp(clamp(-k * (origin.y - g_FogHeight.z), -80.0, 80.0));
    float kv = k * dir.y;
    float integral = abs(kv) > 1e-5 ? (exp(clamp(-kv * s0, -80.0, 80.0)) - exp(clamp(-kv * s1, -80.0, 80.0))) / kv
                                    : s1 - s0;
    return base * max(integral, 0.0);
}

// Adds the local fog volumes at p.
void AddFogVolumes(float3 p, inout float extinction, inout float3 scattering, inout float3 emission)
{
    for (uint i = 0; i < g_FogCounts.x; ++i)
    {
        float3 b = mul(g_FogVolumes[i].WorldToBox, float4(p, 1.0)).xyz;
        float3 a = abs(b);
        if (any(a > 0.5))
            continue;
        // Soft at the sides and the top; the bottom rests on the ground.
        float edge = max(g_FogVolumes[i].Shape.z, 1e-3) * 0.5;
        float fade = smoothstep(0.0, 1.0, saturate((0.5 - a.x) / edge)) *
                     smoothstep(0.0, 1.0, saturate((0.5 - a.z) / edge)) *
                     smoothstep(0.0, 1.0, saturate((0.5 - b.y) / edge));
        float height = (b.y + 0.5) * g_FogVolumes[i].Shape.y;
        float sigma = g_FogVolumes[i].Medium.a * exp(-g_FogVolumes[i].Shape.x * height) * fade;
        extinction += sigma;
        scattering += sigma * g_FogVolumes[i].Medium.rgb;
        emission += g_FogVolumes[i].Emissive.rgb * fade;
    }
}

// The sky's average radiance (the isotropic part of what the fog scatters).
float3 FogAmbient()
{
    return (EnvIrradiance(0, float3(0.0, 1.0, 0.0)) + EnvIrradiance(0, float3(0.0, -1.0, 0.0))) /
           (2.0 * F3D_PI) * g_FogLight.x;
}

// Radiance the directional lights scatter towards V (unit, towards the eye) per unit of
// scattering coefficient; `shadow` applies to the shadowing light.
float3 FogDirectional(float3 V, float shadow)
{
    float3 L = 0.0;
    for (uint d = 0; d < g_LightCounts.y; ++d)
    {
        float visibility = d + 1 == g_LightCounts.z ? shadow : 1.0;
        L += g_DirLightColor[d].rgb * HenyeyGreenstein(dot(-g_DirLightDirection[d].xyz, V), g_FogAlbedo.w) *
             g_FogDirScatter[d] * visibility;
    }
    return L * g_FogLight.y;
}

#endif // F3D_FOG_HLSLI
