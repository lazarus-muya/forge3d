// SPDX-License-Identifier: Apache-2.0
// Irradiance spherical harmonics (phases.md P4.3): projects a radiance cube onto L2
// SH and convolves with the cosine lobe (Ramamoorthi & Hanrahan), so shading
// evaluates irradiance E(n) = sum c_i Y_i(n). Writes 9 float4 at slot * 9, radiance
// decoded from F3D_ENV_SCALE. One group of 256 threads.
#include "atmosphere.hlsli"
#include "ibl.hlsli"

TextureCube<float4> g_Source;
SamplerState        g_Source_sampler;
RWStructuredBuffer<float4> g_SH;

#define F3D_SH_SIZE 32 // texels per face edge sampled
#define F3D_SH_THREADS 256

groupshared float3 s_Sh[F3D_SH_THREADS][9];

[numthreads(F3D_SH_THREADS, 1, 1)]
void main(uint3 id : SV_GroupThreadID)
{
    float acc[27];
    for (uint k = 0; k < 27; ++k)
        acc[k] = 0.0;
    // Sample the mip whose size is closest to F3D_SH_SIZE.
    float mip = max(log2(g_FilterParams.z / F3D_SH_SIZE), 0.0);
    const uint total = 6 * F3D_SH_SIZE * F3D_SH_SIZE;
    for (uint t = id.x; t < total; t += F3D_SH_THREADS)
    {
        uint face = t / (F3D_SH_SIZE * F3D_SH_SIZE);
        uint texel = t % (F3D_SH_SIZE * F3D_SH_SIZE);
        float2 xy = float2(texel % F3D_SH_SIZE, texel / F3D_SH_SIZE);
        float3 dir = CubeDirection(face, xy, F3D_SH_SIZE);
        float w = CubeTexelSolidAngle(xy, F3D_SH_SIZE);
        float3 L = g_Source.SampleLevel(g_Source_sampler, dir, mip).rgb / F3D_ENV_SCALE;
        float b[9];
        ShBasis(dir, b);
        for (uint i = 0; i < 9; ++i)
        {
            acc[i * 3 + 0] += L.r * b[i] * w;
            acc[i * 3 + 1] += L.g * b[i] * w;
            acc[i * 3 + 2] += L.b * b[i] * w;
        }
    }
    for (uint i = 0; i < 9; ++i)
        s_Sh[id.x][i] = float3(acc[i * 3], acc[i * 3 + 1], acc[i * 3 + 2]);
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = F3D_SH_THREADS / 2; stride > 0; stride >>= 1)
    {
        if (id.x < stride)
        {
            for (uint i = 0; i < 9; ++i)
                s_Sh[id.x][i] += s_Sh[id.x + stride][i];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (id.x == 0)
    {
        // Cosine-lobe convolution: A0 = pi, A1 = 2pi/3, A2 = pi/4.
        const float band[9] = {3.14159265, 2.0943951, 2.0943951, 2.0943951, 0.78539816,
                               0.78539816, 0.78539816, 0.78539816, 0.78539816};
        for (uint i = 0; i < 9; ++i)
            g_SH[g_FilterSlot.x * 9 + i] = float4(s_Sh[0][i] * band[i], 0.0);
    }
}
