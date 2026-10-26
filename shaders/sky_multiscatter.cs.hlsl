// SPDX-License-Identifier: Apache-2.0
// Multiple-scattering LUT (phases.md P4.3; Hillaire 2020, section 5.5): for a height
// and sun zenith angle, the infinite-order scattering contribution Psi_ms per unit
// sun illuminance, from 64 directions on the sphere (isotropic phase).
#include "sky_sampling.hlsli"

RWTexture2D<float4> g_OutLut;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= F3D_MULTISCATTER_SIZE || id.y >= F3D_MULTISCATTER_SIZE)
        return;
    float2 uv = (float2(id.xy) + 0.5) / F3D_MULTISCATTER_SIZE;
    float cosSun = uv.x * 2.0 - 1.0;
    float height = max(uv.y * (TopRadius() - GroundRadius()), 0.01);
    float3 p = float3(0.0, GroundRadius() + height, 0.0);
    float3 sunDir = float3(sqrt(saturate(1.0 - cosSun * cosSun)), cosSun, 0.0);

    const int kSqrtDirections = 8;
    const int kSteps = 20;
    const float kIsotropic = 1.0 / (4.0 * F3D_ATMO_PI);
    float3 secondOrder = 0.0;
    float3 transfer = 0.0;
    for (int i = 0; i < kSqrtDirections; ++i)
    {
        for (int j = 0; j < kSqrtDirections; ++j)
        {
            float cosTheta = 1.0 - 2.0 * (i + 0.5) / kSqrtDirections;
            float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
            float phi = 2.0 * F3D_ATMO_PI * (j + 0.5) / kSqrtDirections;
            float3 dir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));

            float tGround = RaySphere(p, dir, GroundRadius());
            float tTop = RaySphere(p, dir, TopRadius());
            float t = tGround > 0.0 ? tGround : tTop;
            float dt = max(t, 0.0) / kSteps;
            float3 throughput = 1.0;
            float3 L = 0.0;
            float3 F = 0.0;
            for (int s = 0; s < kSteps; ++s)
            {
                float3 pos = p + dir * (s + 0.5) * dt;
                Medium m = MediumAt(HeightOf(pos));
                float3 scattering = m.ScatteringRayleigh + m.ScatteringMie;
                float3 extinction = max(m.Extinction, 1e-6);
                float3 stepT = exp(-extinction * dt);
                float3 S = scattering * SunTransmittance(pos, sunDir) * kIsotropic;
                L += throughput * (S - S * stepT) / extinction;
                F += throughput * (scattering - scattering * stepT) / extinction;
                throughput *= stepT;
            }
            if (tGround > 0.0)
            {
                float3 ground = p + dir * tGround;
                float NoL = saturate(dot(normalize(ground), sunDir));
                L += throughput * SunTransmittance(ground, sunDir) * NoL * g_GroundAlbedo.rgb / F3D_ATMO_PI;
            }
            secondOrder += L;
            transfer += F;
        }
    }
    const float kCount = kSqrtDirections * kSqrtDirections;
    secondOrder /= kCount;
    transfer /= kCount;
    g_OutLut[id.xy] = float4(secondOrder / max(1.0 - transfer, 1e-3), 1.0);
}
