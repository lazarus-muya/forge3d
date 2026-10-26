// SPDX-License-Identifier: Apache-2.0
// GGX prefiltered specular cube (phases.md P4.3): one mip per dispatch, roughness
// rising with the mip. Filtered importance sampling (Krivanek & Colbert): each
// sample reads the source mip that matches its pdf, so few samples stay smooth.
#include "ibl.hlsli"

TextureCube<float4> g_Source; // with a full mip chain
SamplerState        g_Source_sampler;
RWTexture2DArray<float4> g_OutCube;

float2 Hammersley(uint i, uint n)
{
    uint bits = reversebits(i);
    return float2((i + 0.5) / n, bits * 2.3283064365386963e-10);
}

float3 ImportanceSampleGgx(float2 xi, float a, float3 N)
{
    float phi = 2.0 * F3D_IBL_PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float3 h = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);
    return normalize(T * h.x + B * h.y + N * h.z);
}

float D_Ggx(float NoH, float a)
{
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (F3D_IBL_PI * d * d);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float size = g_FilterParams.y;
    if (id.x >= (uint)size || id.y >= (uint)size)
        return;
    float3 N = CubeDirection(id.z, float2(id.xy), size);
    float roughness = g_FilterParams.x;
    if (roughness <= 0.0)
    {
        g_OutCube[id] = float4(g_Source.SampleLevel(g_Source_sampler, N, 0).rgb, 1.0);
        return;
    }
    float a = roughness * roughness;
    float sourceSize = g_FilterParams.z;
    float maxMip = g_FilterParams.w - 1.0;
    float texelSolidAngle = 4.0 * F3D_IBL_PI / (6.0 * sourceSize * sourceSize);

    const uint kSamples = 64;
    float3 sum = 0.0;
    float weight = 0.0;
    for (uint i = 0; i < kSamples; ++i)
    {
        float3 H = ImportanceSampleGgx(Hammersley(i, kSamples), a, N);
        float3 L = reflect(-N, H); // V = N
        float NoL = dot(N, L);
        if (NoL <= 0.0)
            continue;
        float NoH = saturate(dot(N, H));
        float pdf = D_Ggx(NoH, a) * 0.25; // D * NoH / (4 VoH), V = N
        float sampleSolidAngle = 1.0 / (kSamples * pdf + 1e-6);
        float mip = clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0, maxMip);
        sum += g_Source.SampleLevel(g_Source_sampler, L, mip).rgb * NoL;
        weight += NoL;
    }
    g_OutCube[id] = float4(sum / max(weight, 1e-6), 1.0);
}
