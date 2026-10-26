// SPDX-License-Identifier: Apache-2.0
#include "view.hlsli"

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

struct PSOutput
{
    float4 Color : SV_Target;
    float  Depth : SV_Depth;
};

// Anti-aliased line intensity for a grid of the given spacing (1 on a line, 0 between).
float GridLines(float2 coord, float spacing, out float density)
{
    float2 c     = coord / spacing;
    float2 width = fwidth(c);
    float2 g     = abs(frac(c - 0.5) - 0.5) / max(width, 1e-6);
    density      = max(width.x, width.y); // grid cells per pixel
    return 1.0 - min(min(g.x, g.y), 1.0);
}

PSOutput main(PSInput input)
{
    float4 nearH = mul(g_InvViewProj, float4(input.Ndc, 0.0, 1.0));
    float4 farH  = mul(g_InvViewProj, float4(input.Ndc, 1.0, 1.0));
    float3 nearP = nearH.xyz / nearH.w;
    float3 farP  = farH.xyz / farH.w;
    float3 dir   = farP - nearP;
    if (abs(dir.y) < 1e-6)
        discard;
    float t = -nearP.y / dir.y;
    if (t <= 0.0 || t >= 1.0)
        discard;
    float3 p = nearP + t * dir;

    float minorDensity, majorDensity;
    float minor = GridLines(p.xz, g_GridParams.z, minorDensity);
    float major = GridLines(p.xz, g_GridParams.w, majorDensity);
    // Fade a level out as its lines get closer than ~2 pixels apart.
    minor *= saturate(1.0 - minorDensity * 2.0);
    major *= saturate(1.0 - majorDensity * 2.0);

    float3 color = float3(0.30, 0.31, 0.33);
    float  alpha = max(minor * 0.35, major * 0.65);
    if (major * 0.65 >= minor * 0.35)
        color = float3(0.40, 0.41, 0.44);

    // World axes: X (z = 0) red, Z (x = 0) blue.
    float2 axisWidth = fwidth(p.xz) * 1.5;
    if (abs(p.z) < axisWidth.y)
    {
        color = float3(0.60, 0.12, 0.12);
        alpha = 0.95;
    }
    if (abs(p.x) < axisWidth.x)
    {
        color = float3(0.12, 0.25, 0.65);
        alpha = 0.95;
    }

    // Fade with distance from the camera and at grazing angles.
    float dist = length(p - g_CameraPos.xyz);
    alpha *= 1.0 - smoothstep(g_GridParams.x, g_GridParams.y, dist);
    float3 viewDir = normalize(dir);
    alpha *= saturate(abs(viewDir.y) * 8.0);
    if (alpha <= 0.002)
        discard;

    float4 clip = mul(g_ViewProj, float4(p, 1.0));
    PSOutput output;
    output.Color = float4(LinearToSrgbOverlay(color), alpha); // drawn after tonemapping
    output.Depth = clip.z / clip.w;
    return output;
}
