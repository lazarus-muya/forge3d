// SPDX-License-Identifier: Apache-2.0
// Water surfaces (phases.md P7.3): shared by water.vs and water.ps.
#ifndef F3D_WATER_HLSLI
#define F3D_WATER_HLSLI

#define F3D_WATER_LAKE  0u
#define F3D_WATER_RIVER 1u // flow-mapped detail, no swell
#define F3D_WATER_OCEAN 2u // a grid around the camera

// f3d::render::raster::WaterConstants.
cbuffer WaterConstants
{
    float4 g_WaterShallow; // rgb: linear shallow colour, a: clarity (metres)
    float4 g_WaterDeep;    // rgb: linear deep colour, a: foam
    float4 g_WaterWaves;   // x: height, y: length, z: speed, w: wind direction (radians)
    float4 g_WaterParams;  // x: refraction, y: roughness, z: time (seconds), w: F3D_WATER_* kind
    float4 g_WaterOcean;   // xy: grid centre (world x, z), z: water level, w: unused
};

// Ripple detail, tiling: rg the slope (0.5 flat), b a foam pattern, a height.
Texture2D    g_WaterDetail;
SamplerState g_WaterDetail_sampler;

static const uint F3D_WATER_WAVES = 4;

// Gerstner waves around the wind direction: displacement (xyz), and the surface normal
// and a crest amount (0..1) at the displaced point.
void Gerstner(float2 xz, out float3 offset, out float3 normal, out float crest)
{
    offset = 0.0;
    normal = float3(0.0, 1.0, 0.0);
    crest = 0.0;
    float height = g_WaterWaves.x;
    if (height <= 0.0 || (uint)g_WaterParams.w == F3D_WATER_RIVER)
        return;
    const float angles[F3D_WATER_WAVES] = {0.0, 0.55, -0.42, 1.05};
    const float lengths[F3D_WATER_WAVES] = {1.0, 0.61, 0.43, 1.73};
    const float amplitudes[F3D_WATER_WAVES] = {0.42, 0.24, 0.14, 0.2};
    float3 n = 0.0;
    [unroll] for (uint i = 0; i < F3D_WATER_WAVES; ++i)
    {
        float a = g_WaterWaves.w + angles[i];
        float2 d = float2(cos(a), sin(a));
        float l = max(g_WaterWaves.y * lengths[i], 0.1);
        float k = 6.2831853 / l;
        float c = sqrt(9.81 / k) * g_WaterWaves.z;
        float amplitude = height * amplitudes[i];
        float q = saturate(0.6 / (k * max(amplitude, 1e-4) * F3D_WATER_WAVES)); // steepness, no loops
        float f = k * (dot(d, xz) - c * g_WaterParams.z);
        float s = sin(f);
        float co = cos(f);
        offset += float3(q * amplitude * d.x * co, amplitude * s, q * amplitude * d.y * co);
        n += float3(d.x * k * amplitude * co, q * k * amplitude * s, d.y * k * amplitude * co);
        crest += s * amplitudes[i];
    }
    normal = normalize(float3(-n.x, 1.0 - n.y, -n.z));
    crest = saturate(crest / 0.6);
}

#endif // F3D_WATER_HLSLI
