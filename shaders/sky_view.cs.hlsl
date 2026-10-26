// SPDX-License-Identifier: Apache-2.0
// Sky-view LUT (phases.md P4.3): the sky's radiance around the camera (single
// scattering + the multiple-scattering LUT + the lit ground below the horizon),
// times F3D_ENV_SCALE. Recomputed each frame the sky is visible.
#define F3D_SKY_USE_MULTISCATTER
#include "sky_sampling.hlsli"

RWTexture2D<float4> g_OutLut;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= F3D_SKYVIEW_W || id.y >= F3D_SKYVIEW_H)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(F3D_SKYVIEW_W, F3D_SKYVIEW_H);
    float3 sunDir = g_SkySunDirection.xyz;
    float3 dir = SkyViewDirection(uv, sunDir);
    float3 p = CameraPlanetPos();

    float tGround = RaySphere(p, dir, GroundRadius());
    float tTop = RaySphere(p, dir, TopRadius());
    float t = tGround > 0.0 ? tGround : tTop;

    float cosView = dot(dir, sunDir);
    float phaseR = RayleighPhase(cosView);
    float phaseM = MiePhase(cosView, g_MieExtinction.w);

    // Samples spaced quadratically: dense near the camera where the air is thick.
    const int kSteps = 32;
    float3 throughput = 1.0;
    float3 L = 0.0;
    float previous = 0.0;
    for (int s = 0; s < kSteps; ++s)
    {
        float x = (s + 1.0) / kSteps;
        float next = max(t, 0.0) * x * x;
        float dt = next - previous;
        float3 pos = p + dir * (previous + 0.5 * dt);
        previous = next;
        Medium m = MediumAt(HeightOf(pos));
        float3 extinction = max(m.Extinction, 1e-6);
        float3 stepT = exp(-extinction * dt);
        float3 sunT = SunTransmittance(pos, sunDir);
        float3 S = (m.ScatteringRayleigh * phaseR + m.ScatteringMie * phaseM) * sunT +
                   (m.ScatteringRayleigh + m.ScatteringMie) * MultiScattering(pos, sunDir);
        L += throughput * (S - S * stepT) / extinction;
        throughput *= stepT;
    }
    float3 radiance = L * g_SkySunIlluminance.rgb;
    if (tGround > 0.0)
        radiance += throughput * g_GroundRadiance.rgb;
    else
        radiance += g_NightRadiance.rgb;
    g_OutLut[id.xy] = float4(radiance * F3D_ENV_SCALE, 1.0);
}
