// SPDX-License-Identifier: Apache-2.0
// Bloom, upsampling: a 3x3 tent filter of the smaller level, added (additive blend) to
// the next larger one, so each level ends up holding the sum of every level below it.
#include "bloom.hlsli"

float4 main(PSInput input) : SV_Target
{
    float2 uv = input.Pos.xy * g_BloomTarget.xy;
    float2 t = g_BloomSource.xy * g_BloomTarget.z;
    float3 s = g_Source.SampleLevel(g_Source_sampler, uv, 0.0).rgb * 4.0;
    s += (g_Source.SampleLevel(g_Source_sampler, uv + float2(-t.x, 0.0), 0.0).rgb +
          g_Source.SampleLevel(g_Source_sampler, uv + float2(t.x, 0.0), 0.0).rgb +
          g_Source.SampleLevel(g_Source_sampler, uv + float2(0.0, -t.y), 0.0).rgb +
          g_Source.SampleLevel(g_Source_sampler, uv + float2(0.0, t.y), 0.0).rgb) * 2.0;
    s += g_Source.SampleLevel(g_Source_sampler, uv + float2(-t.x, -t.y), 0.0).rgb +
         g_Source.SampleLevel(g_Source_sampler, uv + float2(t.x, -t.y), 0.0).rgb +
         g_Source.SampleLevel(g_Source_sampler, uv + float2(-t.x, t.y), 0.0).rgb +
         g_Source.SampleLevel(g_Source_sampler, uv + float2(t.x, t.y), 0.0).rgb;
    return float4(s / 16.0, 1.0);
}
