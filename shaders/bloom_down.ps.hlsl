// SPDX-License-Identifier: Apache-2.0
// Bloom, downsampling (Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare"): a 13-tap filter into the next, half-sized level. The first pass
// (from the scene colour) averages its groups weighted by 1 / (1 + luma) so single
// bright pixels don't flicker, and applies the optional threshold.
#include "bloom.hlsli"

float3 Tap(float2 uv)
{
    return g_Source.SampleLevel(g_Source_sampler, uv, 0.0).rgb;
}

float KarisWeight(float3 c)
{
    return 1.0 / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722)));
}

float4 main(PSInput input) : SV_Target
{
    float2 uv = input.Pos.xy * g_BloomTarget.xy;
    float2 t = g_BloomSource.xy;
    float3 a = Tap(uv + t * float2(-2.0, -2.0));
    float3 b = Tap(uv + t * float2(0.0, -2.0));
    float3 c = Tap(uv + t * float2(2.0, -2.0));
    float3 d = Tap(uv + t * float2(-2.0, 0.0));
    float3 e = Tap(uv);
    float3 f = Tap(uv + t * float2(2.0, 0.0));
    float3 g = Tap(uv + t * float2(-2.0, 2.0));
    float3 h = Tap(uv + t * float2(0.0, 2.0));
    float3 i = Tap(uv + t * float2(2.0, 2.0));
    float3 j = Tap(uv + t * float2(-1.0, -1.0));
    float3 k = Tap(uv + t * float2(1.0, -1.0));
    float3 l = Tap(uv + t * float2(-1.0, 1.0));
    float3 m = Tap(uv + t * float2(1.0, 1.0));
    float3 color;
    if (g_BloomSource.z > 0.5)
    {
        // Five overlapping groups, each weighted by its brightness.
        float3 g0 = (j + k + l + m) * 0.25;
        float3 g1 = (a + b + d + e) * 0.25;
        float3 g2 = (b + c + e + f) * 0.25;
        float3 g3 = (d + e + g + h) * 0.25;
        float3 g4 = (e + f + h + i) * 0.25;
        float w0 = KarisWeight(g0) * 0.5;
        float w1 = KarisWeight(g1) * 0.125;
        float w2 = KarisWeight(g2) * 0.125;
        float w3 = KarisWeight(g3) * 0.125;
        float w4 = KarisWeight(g4) * 0.125;
        color = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
        if (g_BloomSource.w > 0.0)
        {
            float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
            color *= max(luma - g_BloomSource.w, 0.0) / max(luma, 1e-4);
        }
    }
    else
    {
        color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    return float4(max(color, 0.0), 1.0);
}
