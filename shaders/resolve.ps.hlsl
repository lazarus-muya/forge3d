// SPDX-License-Identifier: Apache-2.0
// Viewport resolve (phases.md P4.4): pre-exposed HDR scene colour -> tonemapped,
// sRGB-encoded 8-bit display texture (shown by ImGui and written by --capture).
// The CPU reference is f3d::render::raster::tonemap (render_settings.cpp).
//
// Optionally (g_ResolveMode.z flags): the scene was rendered below the output size and
// is upscaled here (Catmull-Rom), bloom is mixed in, and the PostProcess grading applies
// (white balance, saturation, contrast, vignette before tonemapping; grain after).

Texture2D<float4> g_SceneColor;
SamplerState      g_SceneColor_sampler;
Texture2D<float4> g_Bloom;
SamplerState      g_Bloom_sampler;

#define F3D_RESOLVE_UPSCALE 1u
#define F3D_RESOLVE_BLOOM   2u
#define F3D_RESOLVE_GRADE   4u

cbuffer ResolveConstants
{
    uint4  g_ResolveMode;     // x: tonemapper (0 AgX, 1 ACES, 2 none), y: 1 = sRGB encode, 0 = raw, z: flags, w: frame index
    float4 g_ClearColor;      // linear: where nothing was drawn (scene alpha 0), not tonemapped
    float4 g_BloomParams;     // x: intensity, y: 1 / levels
    float4 g_Grade;           // x: vignette, y: saturation, z: contrast, w: film grain
    float4 g_WhiteBalance[3]; // rows of the linear-sRGB white-balance matrix
    float4 g_Lens;            // xy: tangent of half the field of view (x, y; 0: orthographic), zw: scene texel size
};

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// AgX (Troy Sobotka), Benjamin Wrensch's minimal fit.
float3 AgxContrast(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 Agx(float3 v)
{
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    v = mul(max(v, 1e-10), inset);
    v = clamp(log2(v), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    v = mul(AgxContrast(v), outset);
    return saturate(pow(max(v, 0.0), 2.2));
}

// ACES RRT + ODT fit (Stephen Hill).
float3 Aces(float3 c)
{
    const float3x3 input = float3x3(0.59719, 0.35458, 0.04823, 0.07600, 0.90834, 0.01566, 0.02840, 0.13383, 0.83777);
    const float3x3 output = float3x3(1.60475, -0.53108, -0.07367, -0.10208, 1.10813, -0.00605, -0.00327, -0.07276,
                                     1.07602);
    float3 v = mul(input, c);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return saturate(mul(output, a / b));
}

// Bicubic Catmull-Rom from 9 bilinear taps (the corners dropped: 5 taps).
float4 SampleCatmullRom(float2 uv)
{
    float2 texel = g_Lens.zw;
    float2 position = uv / texel;
    float2 centre = floor(position - 0.5) + 0.5;
    float2 f = position - centre;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (centre - 1.0) * texel;
    float2 t3 = (centre + 2.0) * texel;
    float2 t12 = (centre + w2 / w12) * texel;
    float3 c = 0.0;
    float weight = 0.0;
    c += g_SceneColor.SampleLevel(g_SceneColor_sampler, float2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y);
    c += g_SceneColor.SampleLevel(g_SceneColor_sampler, float2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y);
    c += g_SceneColor.SampleLevel(g_SceneColor_sampler, t12, 0.0).rgb * (w12.x * w12.y);
    c += g_SceneColor.SampleLevel(g_SceneColor_sampler, float2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y);
    c += g_SceneColor.SampleLevel(g_SceneColor_sampler, float2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    // The background flag (alpha) is not filtered across edges.
    float alpha = g_SceneColor.SampleLevel(g_SceneColor_sampler, uv, 0.0).a;
    return float4(max(c / weight, 0.0), alpha);
}

float Hash(float2 p, float frame)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031 + frame * 0.6180339);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 Grade(float3 color, float2 ndc)
{
    color = float3(dot(g_WhiteBalance[0].rgb, color), dot(g_WhiteBalance[1].rgb, color),
                   dot(g_WhiteBalance[2].rgb, color));
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = max(lerp(luma, color, g_Grade.y), 0.0);
    // Contrast around middle grey, in log space.
    color = 0.18 * pow(max(color / 0.18, 0.0), g_Grade.z);
    // Natural vignetting: the cos^4 law of the angle off the lens axis.
    float t2 = dot(ndc * g_Lens.xy, ndc * g_Lens.xy);
    float cos2 = 1.0 / (1.0 + t2);
    return color * lerp(1.0, cos2 * cos2, g_Grade.x);
}

float4 main(PSInput input) : SV_Target
{
    uint flags = g_ResolveMode.z;
    float2 uv = (input.Ndc * float2(0.5, -0.5)) + 0.5;
    float4 scene = (flags & F3D_RESOLVE_UPSCALE) != 0 ? SampleCatmullRom(uv)
                                                      : g_SceneColor.Load(int3(input.Pos.xy, 0));
    float3 color = scene.rgb;
    if (scene.a >= 0.5)
    {
        if ((flags & F3D_RESOLVE_BLOOM) != 0)
            color = lerp(color, g_Bloom.SampleLevel(g_Bloom_sampler, uv, 0.0).rgb * g_BloomParams.y, g_BloomParams.x);
        if ((flags & F3D_RESOLVE_GRADE) != 0)
            color = Grade(color, input.Ndc);
    }
    if (scene.a < 0.5)
        color = g_ClearColor.rgb; // the background is UI colour, not scene light
    else if (g_ResolveMode.x == 0)
        color = Agx(color);
    else if (g_ResolveMode.x == 1)
        color = Aces(color);
    else
        color = saturate(color);
    if ((flags & F3D_RESOLVE_GRADE) != 0 && g_Grade.w > 0.0 && scene.a >= 0.5)
    {
        // Film grain: stronger in the mid-tones, like silver halide.
        float noise = Hash(input.Pos.xy, (float)g_ResolveMode.w) - 0.5;
        float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
        color = saturate(color + noise * g_Grade.w * 0.12 * sqrt(saturate(luma * (1.0 - luma)) * 4.0));
    }
    return float4(g_ResolveMode.y != 0 ? LinearToSrgb(color) : color, 1.0);
}
