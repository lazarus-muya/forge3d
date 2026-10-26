// SPDX-License-Identifier: Apache-2.0
// Sky background (phases.md P4.3): the sky-view LUT plus the sun and moon discs,
// pre-exposed like the rest of the scene colour.
#include "sky_sampling.hlsli"
#include "view.hlsli"

Texture2D<float4> g_SkyViewLut;
SamplerState      g_SkyViewLut_sampler;

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float3 dir;
    if (g_CameraPos.w > 0.5)
    {
        dir = -normalize(g_View[2].xyz); // orthographic: every pixel looks along the camera's -Z
    }
    else
    {
        float4 farPoint = mul(g_InvViewProj, float4(input.Ndc, 1.0, 1.0));
        dir = normalize(farPoint.xyz / farPoint.w - g_CameraPos.xyz);
    }
    float3 sunDir = g_SkySunDirection.xyz;
    float3 L = g_SkyViewLut.SampleLevel(g_SkyViewLut_sampler, SkyViewUv(dir, sunDir), 0).rgb / F3D_ENV_SCALE;

    float3 p = CameraPlanetPos();
    if (RaySphere(p, dir, GroundRadius()) < 0.0)
    {
        float3 T = TransmittanceToTop(p, dir);
        // Sun disc: illuminance spread over its solid angle, with limb darkening.
        float cosSun = dot(dir, sunDir);
        float cosRadius = g_SkySunDirection.w;
        if (cosSun > cosRadius)
        {
            float solidAngle = 2.0 * F3D_ATMO_PI * (1.0 - cosRadius);
            float x = saturate((1.0 - cosSun) / (1.0 - cosRadius)); // 0 centre .. 1 edge (squared radius)
            float limb = 1.0 - 0.6 * (1.0 - sqrt(saturate(1.0 - x)));
            L += T * g_SkySunIlluminance.rgb / solidAngle * limb;
        }
        float cosMoon = dot(dir, g_MoonDirection.xyz);
        if (cosMoon > g_MoonDirection.w)
            L += T * g_MoonRadiance.rgb;
    }
    float3 color = L * FrameExposure();
    return float4(min(color, 60000.0), 1.0);
}
