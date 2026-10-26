// SPDX-License-Identifier: Apache-2.0
// Exposure from the histogram (or the manual EV100), adapted towards the target over
// time, written for the next frame: x exposure, y EV100, z 1 = valid. Clears the
// histogram for the next frame. One group.
#include "exposure.hlsli"

RWStructuredBuffer<float4> g_ExposureState;
RWStructuredBuffer<uint>   g_Histogram;

groupshared uint s_Bins[F3D_HISTOGRAM_BINS];

[numthreads(F3D_HISTOGRAM_BINS, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    s_Bins[index] = g_Histogram[index];
    GroupMemoryBarrierWithGroupSync();
    g_Histogram[index] = 0;
    if (index != 0)
        return;

    float4 previous = g_ExposureState[0];
    float compensation = g_ExposureExtra.x;
    float ev;
    if (g_ExposureLimits.w < 0.5)
    {
        ev = g_ExposureLimits.z - compensation;
    }
    else
    {
        // Mean log2 luminance of the pixels between the low and high percentiles.
        uint total = 0;
        for (uint b = 0; b < F3D_HISTOGRAM_BINS; ++b)
            total += s_Bins[b];
        if (total == 0)
            return; // nothing rendered: keep the exposure
        float low = g_ExposureExtra.y * total;
        float high = g_ExposureExtra.z * total;
        float seen = 0.0;
        float sum = 0.0;
        float weight = 0.0;
        for (uint i = 0; i < F3D_HISTOGRAM_BINS; ++i)
        {
            float count = (float)s_Bins[i];
            // The part of this bin inside [low, high].
            float inside = max(min(seen + count, high) - max(seen, low), 0.0);
            seen += count;
            if (inside <= 0.0)
                continue;
            float log2L = i == 0 ? g_ExposureParams.x - 1.0
                                 : g_ExposureParams.x + ((i - 0.5) / 254.0) * g_ExposureParams.y;
            sum += log2L * inside;
            weight += inside;
        }
        float averageLog2 = weight > 0.0 ? sum / weight : g_ExposureParams.x;
        // Reflected-light meter: EV100 = log2(L * 100 / 12.5).
        float target = clamp(averageLog2 + 3.0 - compensation, g_ExposureLimits.x, g_ExposureLimits.y);
        if (g_ExposureExtra.w > 0.5 || previous.z < 0.5)
            ev = target;
        else
            ev = lerp(previous.y, target, target > previous.y ? g_ExposureParams.z : g_ExposureParams.w);
    }
    g_ExposureState[0] = float4(1.0 / (1.2 * exp2(ev)), ev, 1.0, 0.0);
}
