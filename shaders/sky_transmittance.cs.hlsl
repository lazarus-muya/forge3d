// SPDX-License-Identifier: Apache-2.0
// Transmittance LUT (phases.md P4.3): transmittance from (r, mu) to the top of the
// atmosphere. Rays that hit the ground are not looked up here (the caller tests).
#include "atmosphere.hlsli"

RWTexture2D<float4> g_OutLut;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= F3D_TRANSMITTANCE_W || id.y >= F3D_TRANSMITTANCE_H)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(F3D_TRANSMITTANCE_W, F3D_TRANSMITTANCE_H);
    float r, mu;
    TransmittanceRMu(uv, r, mu);
    float3 origin = float3(0.0, r, 0.0);
    float3 dir = float3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
    float length = RaySphere(origin, dir, TopRadius());
    const int kSteps = 40;
    float dt = max(length, 0.0) / kSteps;
    float3 depth = 0.0;
    for (int i = 0; i < kSteps; ++i)
        depth += MediumAt(HeightOf(origin + dir * (i + 0.5) * dt)).Extinction * dt;
    g_OutLut[id.xy] = float4(exp(-depth), 1.0);
}
