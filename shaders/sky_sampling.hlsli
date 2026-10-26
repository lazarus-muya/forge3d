// SPDX-License-Identifier: Apache-2.0
// Transmittance and multiple-scattering look-ups shared by the sky passes.
#ifndef F3D_SKY_SAMPLING_HLSLI
#define F3D_SKY_SAMPLING_HLSLI

#include "atmosphere.hlsli"

Texture2D<float4> g_TransmittanceLut;
SamplerState      g_TransmittanceLut_sampler;

// Transmittance from planet-centred p along dir to the top of the atmosphere.
float3 TransmittanceToTop(float3 p, float3 dir)
{
    float r = length(p);
    float mu = dot(p / r, dir);
    return g_TransmittanceLut.SampleLevel(g_TransmittanceLut_sampler, TransmittanceUv(r, mu), 0).rgb;
}

// Sunlight reaching p: zero when the planet is in the way.
float3 SunTransmittance(float3 p, float3 sunDir)
{
    if (RaySphere(p, sunDir, GroundRadius()) > 0.0)
        return 0.0;
    return TransmittanceToTop(p, sunDir);
}

#ifdef F3D_SKY_USE_MULTISCATTER
Texture2D<float4> g_MultiScatterLut;
SamplerState      g_MultiScatterLut_sampler;

// Psi_ms per unit sun illuminance (Hillaire, eq. 10).
float3 MultiScattering(float3 p, float3 sunDir)
{
    float r = length(p);
    float cosSun = dot(p / r, sunDir);
    float2 uv = float2(cosSun * 0.5 + 0.5, saturate((r - GroundRadius()) / (TopRadius() - GroundRadius())));
    return g_MultiScatterLut.SampleLevel(g_MultiScatterLut_sampler, uv, 0).rgb;
}
#endif

#endif // F3D_SKY_SAMPLING_HLSLI
