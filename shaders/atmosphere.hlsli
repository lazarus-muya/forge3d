// SPDX-License-Identifier: Apache-2.0
// Physical atmosphere (phases.md P4.3), after Hillaire, "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique" (EGSR 2020): transmittance,
// multiple-scattering and sky-view look-up tables. Distances in km, the planet's
// centre below the camera; world directions (+Y up). f3d::render::raster
// (atmosphere.hpp) holds the CPU reference of the same model.
#ifndef F3D_ATMOSPHERE_HLSLI
#define F3D_ATMOSPHERE_HLSLI

// f3d::render::raster::SkyConstants.
cbuffer SkyConstants
{
    float4 g_RayleighScattering; // rgb: 1/km, w: scale height (km)
    float4 g_MieScattering;      // rgb: 1/km, w: scale height (km)
    float4 g_MieExtinction;      // rgb: 1/km, w: phase asymmetry g
    float4 g_OzoneAbsorption;    // rgb: 1/km, w: layer centre (km)
    float4 g_PlanetParams;       // x: ground radius, y: top radius, z: ozone half-width (km), w: camera height (km)
    float4 g_GroundAlbedo;       // rgb, w: 1 = physical sky, 0 = hemisphere colours
    float4 g_SkySunDirection;    // xyz: towards the sun, w: cos(sun angular radius)
    float4 g_SkySunIlluminance;  // rgb: lux at the top of the atmosphere, w: unused
    float4 g_GroundRadiance;     // rgb: radiance of the planet's surface, w: unused
    float4 g_MoonDirection;      // xyz: towards the moon, w: cos(moon angular radius)
    float4 g_MoonRadiance;       // rgb: radiance of the moon's disc (0: no moon), w: unused
    float4 g_NightRadiance;      // rgb: airglow and starlight, w: unused
    float4 g_HemiSky;            // rgb: hemisphere mode, radiance from above
    float4 g_HemiGround;         // rgb: hemisphere mode, radiance from below
};

static const float F3D_ATMO_PI = 3.14159265;
// Sky textures (sky-view LUT, environment and probe cubes) store radiance times
// this factor so fp16 holds daylight values (f3d::render::raster::kEnvScale).
static const float F3D_ENV_SCALE = 1.0 / 4096.0;

#define F3D_TRANSMITTANCE_W 256
#define F3D_TRANSMITTANCE_H 64
#define F3D_MULTISCATTER_SIZE 32
#define F3D_SKYVIEW_W 192
#define F3D_SKYVIEW_H 108

float GroundRadius() { return g_PlanetParams.x; }
float TopRadius()    { return g_PlanetParams.y; }

// Distance along a ray (origin relative to the planet's centre) to a sphere, or -1.
float RaySphere(float3 origin, float3 dir, float radius)
{
    float b = dot(origin, dir);
    float c = dot(origin, origin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0)
        return -1.0;
    float s = sqrt(d);
    float t0 = -b - s;
    float t1 = -b + s;
    if (t0 >= 0.0)
        return t0;
    return t1 >= 0.0 ? t1 : -1.0;
}

struct Medium
{
    float3 ScatteringRayleigh;
    float3 ScatteringMie;
    float3 Extinction;
};

Medium MediumAt(float height)
{
    float rayleigh = exp(-height / g_RayleighScattering.w);
    float mie      = exp(-height / g_MieScattering.w);
    float ozone    = max(0.0, 1.0 - abs(height - g_OzoneAbsorption.w) / g_PlanetParams.z);
    Medium m;
    m.ScatteringRayleigh = g_RayleighScattering.rgb * rayleigh;
    m.ScatteringMie      = g_MieScattering.rgb * mie;
    m.Extinction         = m.ScatteringRayleigh + g_MieExtinction.rgb * mie + g_OzoneAbsorption.rgb * ozone;
    return m;
}

float RayleighPhase(float c)
{
    return 3.0 / (16.0 * F3D_ATMO_PI) * (1.0 + c * c);
}

// Cornette-Shanks.
float MiePhase(float c, float g)
{
    float g2 = g * g;
    float k = 3.0 / (8.0 * F3D_ATMO_PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + c * c) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}

// Transmittance LUT parameterisation (Bruneton): (r, mu) <-> uv.
float2 TransmittanceUv(float r, float mu)
{
    float H   = sqrt(TopRadius() * TopRadius() - GroundRadius() * GroundRadius());
    float rho = sqrt(max(r * r - GroundRadius() * GroundRadius(), 0.0));
    float discriminant = r * r * (mu * mu - 1.0) + TopRadius() * TopRadius();
    float d    = max(0.0, -r * mu + sqrt(max(discriminant, 0.0)));
    float dMin = TopRadius() - r;
    float dMax = rho + H;
    return float2((d - dMin) / max(dMax - dMin, 1e-6), rho / H);
}

void TransmittanceRMu(float2 uv, out float r, out float mu)
{
    float H   = sqrt(TopRadius() * TopRadius() - GroundRadius() * GroundRadius());
    float rho = H * uv.y;
    r = sqrt(rho * rho + GroundRadius() * GroundRadius());
    float dMin = TopRadius() - r;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

// Height above the ground of a planet-centred position.
float HeightOf(float3 p)
{
    return max(length(p) - GroundRadius(), 0.0);
}

// The camera's planet-centred position.
float3 CameraPlanetPos()
{
    return float3(0.0, GroundRadius() + max(g_PlanetParams.w, 0.001), 0.0);
}

// Sky-view LUT parameterisation: u = azimuth from the sun (0..pi, symmetric),
// v = elevation with extra resolution at the horizon.
float2 SkyViewUv(float3 dir, float3 sunDir)
{
    float elevation = asin(clamp(dir.y, -1.0, 1.0));
    float v = 0.5 + 0.5 * sign(elevation) * sqrt(abs(elevation) / (0.5 * F3D_ATMO_PI));
    float2 d = dir.xz;
    float2 s = sunDir.xz;
    float u = 0.0;
    if (dot(d, d) > 1e-10 && dot(s, s) > 1e-10)
        u = acos(clamp(dot(normalize(d), normalize(s)), -1.0, 1.0)) / F3D_ATMO_PI;
    return float2(u, v);
}

float3 SkyViewDirection(float2 uv, float3 sunDir)
{
    float x = uv.y * 2.0 - 1.0;
    float elevation = sign(x) * x * x * 0.5 * F3D_ATMO_PI;
    float azimuth = uv.x * F3D_ATMO_PI;
    float2 s = dot(sunDir.xz, sunDir.xz) > 1e-10 ? normalize(sunDir.xz) : float2(0.0, -1.0);
    float ca = cos(azimuth), sa = sin(azimuth);
    float2 d = float2(s.x * ca - s.y * sa, s.x * sa + s.y * ca);
    return float3(d.x * cos(elevation), sin(elevation), d.y * cos(elevation));
}

#endif // F3D_ATMOSPHERE_HLSLI
