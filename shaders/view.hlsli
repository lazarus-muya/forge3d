// SPDX-License-Identifier: Apache-2.0
// Per-view constants (f3d::render::raster::ViewConstants). Column-major matrices; mul(M, v).
#ifndef F3D_VIEW_HLSLI
#define F3D_VIEW_HLSLI

#define F3D_MAX_DIRECTIONAL_LIGHTS 4
#define F3D_SHADOW_CASCADES 4

cbuffer ViewConstants
{
    float4x4 g_ViewProj;
    float4x4 g_InvViewProj;
    float4x4 g_View;
    float4   g_CameraPos;  // xyz: camera position, w: 1 if orthographic
    float4   g_Viewport;   // width, height, 1/width, 1/height (pixels)
    float4   g_GridParams; // x: fade start, y: fade end, z: minor spacing, w: major spacing
    float4   g_FrameParams;  // x: exposure scale, y: specular environment mips, z: 1 = times the automatic exposure, w: view mode
    float4x4 g_InvProj;
    float4   g_ClusterParams; // x, y: cluster tile size (pixels), z: depth slice scale, w: bias
    uint4    g_LightCounts;   // x: local lights, y: directional lights, z: shadowing directional light + 1 (0: none)
    float4   g_DirLightDirection[F3D_MAX_DIRECTIONAL_LIGHTS]; // xyz: unit, towards the light
    float4   g_DirLightColor[F3D_MAX_DIRECTIONAL_LIGHTS];     // rgb: illuminance
    float4x4 g_ShadowMatrices[F3D_SHADOW_CASCADES]; // world -> cascade clip space
    float4   g_ShadowSplits; // view depth where each cascade ends
    float4   g_ShadowTexel;  // world size of a shadow-map texel, per cascade
};

// x: the automatic exposure (f3d::render::raster exposure state, written by exposure_adapt).
StructuredBuffer<float4> g_Exposure;

#define F3D_VIEW_LIT       0
#define F3D_VIEW_UNLIT     1
#define F3D_VIEW_WIREFRAME 2
#define F3D_VIEW_NORMALS   3
#define F3D_VIEW_CASCADES  4

uint ViewMode()
{
    return (uint)g_FrameParams.w;
}

// The factor applied to scene radiance before it is written (pre-exposure).
float FrameExposure()
{
    return g_FrameParams.z > 0.5 ? g_FrameParams.x * g_Exposure[0].x : g_FrameParams.x;
}

// sRGB-encoded colour (vertex colours, UI colours) to linear.
float3 SrgbToLinear(float3 c)
{
    return pow(saturate(c), 2.2);
}

// Linear colour to sRGB encoding (overlays drawn after tonemapping).
float3 LinearToSrgbOverlay(float3 c)
{
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

#endif // F3D_VIEW_HLSLI
