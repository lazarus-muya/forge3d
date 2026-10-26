// SPDX-License-Identifier: Apache-2.0
// Luminance histogram of the (pre-exposed) scene colour, in absolute cd/m2: bin 0
// holds black pixels, bins 1..255 log2 luminance over the configured range.
#include "exposure.hlsli"

Texture2D<float4>          g_SceneColor;
StructuredBuffer<float4>   g_Exposure; // x: the exposure the scene colour was rendered with
RWStructuredBuffer<uint>   g_Histogram;

groupshared uint s_Bins[F3D_HISTOGRAM_BINS];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    s_Bins[index] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint width, height;
    g_SceneColor.GetDimensions(width, height);
    if (id.x < width && id.y < height && g_SceneColor[id.xy].a > 0.5) // background (alpha 0) isn't metered
    {
        float3 c = g_SceneColor[id.xy].rgb;
        float luminance = dot(c, float3(0.2126, 0.7152, 0.0722)) / max(g_Exposure[0].x, 1e-12);
        uint bin = 0;
        if (luminance > 1e-6)
        {
            float x = saturate((log2(luminance) - g_ExposureParams.x) / g_ExposureParams.y);
            bin = (uint)(x * 254.0 + 1.0);
        }
        InterlockedAdd(s_Bins[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (s_Bins[index] != 0)
        InterlockedAdd(g_Histogram[index], s_Bins[index]);
}
