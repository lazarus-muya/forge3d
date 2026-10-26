// SPDX-License-Identifier: Apache-2.0
// Water surfaces (phases.md P7.3). Drawn after the opaque scene and the sky, over a copy
// of both (water_copy.ps):
//   - normals: the Gerstner swell (water.vs) plus two scrolling layers of ripple detail
//     (rivers: flow-mapped along the current, two phases cross-faded);
//   - refraction: the scene copy, offset by the ripples where the water is deep enough
//     (not where something in front of the water would bleed in);
//   - absorption: what is seen through the water fades with the distance travelled in
//     it (red first), into the shallow-to-deep scattering colour, lit by the sky and sun;
//   - reflection: screen-space (traced over the depth copy), else the sky's environment,
//     weighted by Fresnel, plus the sun's highlight (shadowed);
//   - foam where the water is shallow (shores) and on crests.
// Everything is in the pre-exposed space of the scene colour.
#include "forward.hlsli"
#include "water.hlsli"

Texture2D        g_WaterColor; // the scene colour copy (pre-exposed)
SamplerState     g_WaterColor_sampler;
Texture2D<float> g_WaterDepth; // the depth copy (device depth)

struct PSInput
{
    float4 Pos      : SV_Position;
    float3 WorldPos : TEXCOORD0;
    float3 Normal   : NORMAL;
    float2 Flow     : TEXCOORD1;
    float  Crest    : TEXCOORD2;
};

float SceneDepthAt(float2 uv)
{
    int2 p = clamp(int2(uv * g_Viewport.xy), int2(0, 0), int2(g_Viewport.xy) - 1);
    return g_WaterDepth.Load(int3(p, 0));
}

float3 WorldAt(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 world = mul(g_InvViewProj, ndc);
    return world.xyz / world.w;
}

float4 Ripples(float2 uv)
{
    return g_WaterDetail.Sample(g_WaterDetail_sampler, uv);
}

// The ripple slope (x/z) and foam pattern at a point.
float3 Detail(float3 world, float2 flow, uint kind)
{
    float time = g_WaterParams.z;
    float scale = 1.0 / max(g_WaterWaves.y * 1.4, 0.5);
    float2 uv = world.xz * scale;
    float4 a, b;
    if (kind == F3D_WATER_RIVER)
    {
        // Flow map: two phases half a cycle apart, each dragged along the flow, cross-faded.
        float cycle = 2.0;
        float p0 = frac(time / cycle);
        float p1 = frac(time / cycle + 0.5);
        float2 drift = flow * scale * cycle;
        float4 s0 = Ripples(uv - drift * p0) * 0.6 + Ripples(uv * 2.3 - drift * p0 * 2.3 + 0.37) * 0.4;
        float4 s1 = Ripples(uv - drift * p1 + 0.5) * 0.6 + Ripples(uv * 2.3 - drift * p1 * 2.3 + 0.87) * 0.4;
        float w = abs(p0 * 2.0 - 1.0);
        a = lerp(s0, s1, w);
        b = a;
    }
    else
    {
        float2 wind = float2(cos(g_WaterWaves.w), sin(g_WaterWaves.w));
        float2 drift = wind * time * 0.04 * g_WaterWaves.z;
        const float2x2 turn = float2x2(0.6, -0.8, 0.8, 0.6);
        a = Ripples(uv + drift);
        b = Ripples(mul(turn, uv) * 0.47 - drift * 0.6 + 0.31);
        // A slow, large third layer that varies the other two (no visible repeats).
        float4 c = Ripples(mul(transpose(turn), uv) * 0.131 + drift * 0.3 + 0.71);
        a.rg = lerp(a.rg, c.rg, 0.35);
        b.rg = 0.5 + (b.rg - 0.5) * (0.6 + c.a * 0.8);
    }
    float2 slope = (a.rg - 0.5) + (b.rg - 0.5);
    return float3(slope, a.b * 0.5 + b.b * 0.5);
}

float3 TraceReflection(float3 origin, float3 dir, float3 fallback, out float confidence)
{
    confidence = 0.0;
    float3 p = origin;
    float3 previous = origin;
    float step = 0.4;
    [loop] for (int i = 0; i < 48; ++i)
    {
        previous = p;
        p += dir * step;
        step *= 1.12;
        float4 clip = mul(g_ViewProj, float4(p, 1.0));
        if (clip.w <= 1e-4)
            break;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0))
            break;
        float sceneZ = SceneDepthAt(uv);
        if (sceneZ < 1.0 && ndc.z > sceneZ)
        {
            // Crossed a surface between the last two points: bisect to it.
            float3 lo = previous;
            float3 hi = p;
            [unroll] for (int r = 0; r < 6; ++r)
            {
                float3 mid = (lo + hi) * 0.5;
                float4 c = mul(g_ViewProj, float4(mid, 1.0));
                float3 n = c.xyz / c.w;
                float2 u = n.xy * float2(0.5, -0.5) + 0.5;
                if (n.z > SceneDepthAt(u))
                    hi = mid;
                else
                    lo = mid;
            }
            float4 c = mul(g_ViewProj, float4(hi, 1.0));
            float2 huv = (c.xy / c.w) * float2(0.5, -0.5) + 0.5;
            float hz = SceneDepthAt(huv);
            float3 surface = WorldAt(huv, hz);
            // A real hit, not a ray passing behind a thin object far in front of it.
            if (length(surface - hi) < max(length(hi - lo) * 4.0, 0.5 + 0.02 * length(hi - origin)))
            {
                float2 edge = min(huv, 1.0 - huv);
                confidence = saturate(min(edge.x, edge.y) * 12.0) * saturate(1.5 - i / 32.0);
                return g_WaterColor.SampleLevel(g_WaterColor_sampler, huv, 0).rgb;
            }
            break;
        }
    }
    return fallback;
}

float4 main(PSInput input) : SV_Target
{
    uint kind = (uint)g_WaterParams.w;
    float3 camera = g_CameraPos.xyz;
    float3 V = g_CameraPos.w > 0.5 ? normalize(g_View[2].xyz) : normalize(camera - input.WorldPos);
    float exposure = FrameExposure();

    // Normal: the swell plus the ripples.
    float3 detail = Detail(input.WorldPos, input.Flow, kind);
    // Ripples fade with distance (their repeats would show; the swell takes over).
    float distanceFade = lerp(1.0, 0.3, saturate(length(camera - input.WorldPos) / 180.0));
    float ripple = (kind == F3D_WATER_RIVER ? 1.6 : 1.0) * distanceFade;
    float3 N = normalize(input.Normal + float3(detail.x, 0.0, detail.y) * ripple);
    float NoV = saturate(dot(N, V));

    uint mode = ViewMode();
    if (mode == F3D_VIEW_NORMALS)
        return float4(N * 0.5 + 0.5, 1.0);
    if (mode == F3D_VIEW_UNLIT)
        return float4(g_WaterShallow.rgb, 1.0);
    if (mode == F3D_VIEW_WIREFRAME)
        return float4(0.4, 0.6, 0.9, 1.0);

    // What is under the water here, and how much water the view crosses to it.
    float2 suv = input.Pos.xy * g_Viewport.zw;
    float sceneZ = SceneDepthAt(suv);
    float surfaceDistance = length(camera - input.WorldPos);
    float3 bed = WorldAt(suv, sceneZ);
    float thickness = sceneZ >= 1.0 ? 1000.0 : max(length(camera - bed) - surfaceDistance, 0.0);
    float depthBelow = sceneZ >= 1.0 ? 1000.0 : max(input.WorldPos.y - bed.y, 0.0);

    // Refraction: offset by the ripples, unless that lands on something above the water.
    float2 ruv = suv + N.xz * g_WaterParams.x * 0.035 * saturate(thickness * 0.4);
    float rz = SceneDepthAt(ruv);
    if (rz < input.Pos.z)
    {
        ruv = suv;
        rz = sceneZ;
    }
    float3 refracted = g_WaterColor.SampleLevel(g_WaterColor_sampler, ruv, 0).rgb;
    float3 rbed = WorldAt(ruv, rz);
    float rthickness = rz >= 1.0 ? 1000.0 : max(length(camera - rbed) - surfaceDistance, 0.0);

    // Light reaching the water: the sky (irradiance) and the sun (shadowed).
    float3 L = float3(0.0, 1.0, 0.0);
    float3 sun = 0.0;
    float viewDepth = -mul(g_View, float4(input.WorldPos, 1.0)).z;
    if (g_LightCounts.y > 0)
    {
        L = g_DirLightDirection[0].xyz;
        sun = g_DirLightColor[0].rgb;
        if (g_LightCounts.z == 1)
            sun *= ShadowFactor(input.WorldPos, float3(0.0, 1.0, 0.0), viewDepth);
    }
    float3 sky = EnvIrradiance(0, float3(0.0, 1.0, 0.0));
    float3 incoming = (sky + sun * saturate(L.y)) / F3D_PI;

    // Absorption and scattering: red goes first, then green; the water's own colour
    // takes over with depth.
    float clarity = max(g_WaterShallow.a, 0.05);
    float3 transmittance = exp(-rthickness / (clarity * float3(0.42, 0.8, 1.0)));
    float3 scatterColor = lerp(g_WaterShallow.rgb, g_WaterDeep.rgb, saturate(rthickness / (clarity * 1.5)));
    float3 body = refracted * transmittance + scatterColor * incoming * exposure * (1.0 - transmittance);

    // Reflection: what the screen shows along the mirrored ray, else the sky.
    float3 R = reflect(-V, N);
    R.y = abs(R.y); // ripples facing away: keep the reflection above the horizon
    float roughness = saturate(g_WaterParams.y);
    float3 env = g_EnvSpecular.SampleLevel(g_EnvSpecular_sampler, R, roughness * max(g_FrameParams.y - 1.0, 0.0)).rgb /
                 F3D_ENV_SCALE * exposure;
    float confidence;
    float3 traced = TraceReflection(input.WorldPos + N * 0.05, R, env, confidence);
    float3 reflection = lerp(env, traced, confidence);
    float fresnel = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);

    // The sun's highlight (GGX).
    float3 H = normalize(L + V);
    float NoL = saturate(dot(N, L));
    float NoH = saturate(dot(N, H));
    float a2 = max(roughness * roughness, 0.002);
    a2 *= a2;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    float D = a2 / (F3D_PI * d * d);
    float3 highlight = sun * NoL * D * fresnel * 0.25 / max(NoV * NoL, 0.05);

    float3 color = lerp(body, reflection, fresnel) + highlight * exposure;

    // Foam on shores and crests, broken up by the ripple pattern.
    float foamAmount = g_WaterDeep.a;
    float shore = 1.0 - saturate(depthBelow / (0.25 + 0.6 * foamAmount));
    float crest = smoothstep(0.55, 0.95, input.Crest);
    float pattern = detail.z;
    float foam = saturate((shore * shore + crest * 0.7) * foamAmount) * smoothstep(0.42, 0.7, pattern + shore * 0.35);
    color = lerp(color, 0.85 * incoming * exposure, saturate(foam));

    // A soft line where the water meets the shore.
    color = lerp(g_WaterColor.SampleLevel(g_WaterColor_sampler, suv, 0).rgb, color, saturate(depthBelow / 0.08));
    return float4(min(color, 60000.0), 1.0);
}
