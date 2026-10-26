// SPDX-License-Identifier: Apache-2.0
// Terrain material layers (phases.md P6.4). Up to 16 layers weighted by the splat maps
// (rules baked by terrain.apply_rules, painted by terrain.paint_stroke). The four
// strongest layers of a pixel are sampled from the layer texture arrays (colour +
// height, normal + roughness + AO), top-down or triplanar, and blended by weight plus
// height ("height blending"), so borders follow the stones and grass instead of a
// soft gradient. Layers without textures get their constants and procedural detail.
// Anti-tiling (per layer): a slow noise picks one of eight shifted copies of the
// textures and blends neighbouring copies, and far away each layer is also sampled
// at a larger, rotated scale, so repeats never line up. Shading is mesh.ps's.
#include "forward.hlsli"
#include "terrain.hlsli"

struct PSInput
{
    float4 Pos      : SV_Position;
    float3 Normal   : NORMAL;
    float3 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
};

float Hash12(float2 p)
{
    float3 q = frac(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

float ValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash12(i);
    float b = Hash12(i + float2(1.0, 0.0));
    float c = Hash12(i + float2(0.0, 1.0));
    float d = Hash12(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Procedural detail in [0, 1] for layers without textures: two octaves of value noise.
float Detail(float2 p)
{
    return ValueNoise(p) * 0.65 + ValueNoise(p * 2.7 + 13.1) * 0.35;
}

struct LayerSample
{
    float3 albedo; // linear
    float  height;
    float3 normal; // world
    float  roughness;
    float  ao;
};

// Tangent normal (along +u, +v, up) from a normal-array texel.
float3 TangentNormal(float4 texel, float strength)
{
    float2 xy = (texel.rg * 2.0 - 1.0) * strength;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}

// Texture variation (Quilez, "Texture repetition", technique 3): the noise index picks
// a copy shifted by a pseudo-random offset; between two indices the copies blend, the
// blend biased by their colour difference so it follows the texture's features.
// `amount` is the share of indices that get an offset (0: the plain texture). Offsets
// only, no rotation, so the normal maps stay valid. Explicit gradients: the offset
// jumps would otherwise break the mip selection.
void SampleVaried(uint layer, float2 uv, float2 dx, float2 dy, float amount, out float4 albedo, out float4 nrm)
{
    float k = ValueNoise(uv * 0.21 + layer * 7.13) * 8.0;
    float i = floor(k);
    float f = frac(k);
    float2 offA = Hash12(float2(i, layer)) < amount ? sin(float2(3.0, 7.0) * i) : 0.0;
    float2 offB = Hash12(float2(i + 1.0, layer)) < amount ? sin(float2(3.0, 7.0) * (i + 1.0)) : 0.0;
    float4 albA = g_LayerAlbedo.SampleGrad(g_LayerAlbedo_sampler, float3(uv + offA, layer), dx, dy);
    float4 albB = g_LayerAlbedo.SampleGrad(g_LayerAlbedo_sampler, float3(uv + offB, layer), dx, dy);
    float4 nrmA = g_LayerNormal.SampleGrad(g_LayerNormal_sampler, float3(uv + offA, layer), dx, dy);
    float4 nrmB = g_LayerNormal.SampleGrad(g_LayerNormal_sampler, float3(uv + offB, layer), dx, dy);
    float3 diff = albA.rgb - albB.rgb;
    float t = smoothstep(0.2, 0.8, f - 0.1 * (diff.r + diff.g + diff.b));
    albedo = lerp(albA, albB, t);
    nrm = lerp(nrmA, nrmB, t);
}

// `dLocalX/Y`: screen derivatives of local.xz, taken in uniform control flow by the caller.
LayerSample SampleLayer(uint layer, float3 local, float3 N, float distance, float2 dLocalX, float2 dLocalY)
{
    LayerSample o;
    float  scale = g_LayerParams[layer].x;
    uint   flags = g_LayerFlags[layer].x;
    bool   triplanar = g_LayerParams[layer].y > 0.5;
    float  strength = g_LayerParams[layer].w;
    float3 tint = g_LayerColor[layer].rgb;

    float4 albedo = 0.0;
    float4 nrm = 0.0;
    float3 normal = N;
    if (triplanar)
    {
        // Triplanar with whiteout normal blending (Golus, "Normal Mapping for a Triplanar Shader").
        float3 w = pow(abs(N), 4.0);
        w /= max(w.x + w.y + w.z, 1e-5);
        float3 axisSign = sign(N);
        float2 uvX = local.zy * scale;
        float2 uvY = local.xz * scale;
        float2 uvZ = local.xy * scale;
        albedo = g_LayerAlbedo.Sample(g_LayerAlbedo_sampler, float3(uvX, layer)) * w.x +
                 g_LayerAlbedo.Sample(g_LayerAlbedo_sampler, float3(uvY, layer)) * w.y +
                 g_LayerAlbedo.Sample(g_LayerAlbedo_sampler, float3(uvZ, layer)) * w.z;
        float4 nX = g_LayerNormal.Sample(g_LayerNormal_sampler, float3(uvX, layer));
        float4 nY = g_LayerNormal.Sample(g_LayerNormal_sampler, float3(uvY, layer));
        float4 nZ = g_LayerNormal.Sample(g_LayerNormal_sampler, float3(uvZ, layer));
        nrm = nX * w.x + nY * w.y + nZ * w.z;
        if ((flags & F3D_LAYER_NORMAL) != 0)
        {
            float3 tX = TangentNormal(nX, strength);
            float3 tY = TangentNormal(nY, strength);
            float3 tZ = TangentNormal(nZ, strength);
            tX.x *= axisSign.x;
            tY.x *= axisSign.y;
            tZ.x *= -axisSign.z;
            tX = float3(tX.xy + N.zy, abs(tX.z) * N.x);
            tY = float3(tY.xy + N.xz, abs(tY.z) * N.y);
            tZ = float3(tZ.xy + N.xy, abs(tZ.z) * N.z);
            normal = normalize(tX.zyx * w.x + tY.xzy * w.y + tZ.xyz * w.z);
        }
    }
    else
    {
        float2 uv = local.xz * scale;
        float2 dx = dLocalX * scale;
        float2 dy = dLocalY * scale;
        float antiTiling = g_LayerParams2[layer].y;
        if (antiTiling > 0.0)
        {
            SampleVaried(layer, uv, dx, dy, antiTiling, albedo, nrm);
            // A second, rotated and larger sample, mixed in by a slow noise (more of it
            // far away, where the grid of repeats shows most).
            const float2x2 turn = float2x2(0.799, -0.602, 0.602, 0.799); // 37 degrees
            float2 uv2 = mul(turn, uv) * 0.43 + float2(0.37, 0.11) * (layer + 1);
            float mask = smoothstep(0.3, 0.7, ValueNoise(local.xz * 0.045 + layer * 3.7));
            float far = saturate((distance * scale - 12.0) / 24.0);
            float mixAmount = saturate(mask * 0.75 + far * 0.35) * saturate(antiTiling * 2.0);
            if (mixAmount > 0.0)
                albedo = lerp(albedo,
                              g_LayerAlbedo.SampleGrad(g_LayerAlbedo_sampler, float3(uv2, layer),
                                                       mul(turn, dx) * 0.43, mul(turn, dy) * 0.43),
                              mixAmount);
        }
        else
        {
            albedo = g_LayerAlbedo.SampleGrad(g_LayerAlbedo_sampler, float3(uv, layer), dx, dy);
            nrm = g_LayerNormal.SampleGrad(g_LayerNormal_sampler, float3(uv, layer), dx, dy);
        }
        if ((flags & F3D_LAYER_NORMAL) != 0)
        {
            float3 t = TangentNormal(nrm, strength);
            normal = normalize(float3(t.x + N.x, abs(t.z) * N.y, t.y + N.z));
        }
    }

    if ((flags & F3D_LAYER_ALBEDO) != 0)
    {
        o.albedo = SrgbToLinear(albedo.rgb) * tint;
        o.height = albedo.a;
    }
    else
    {
        float2 offset = float2(layer * 17.3, layer * 5.9);
        float d = triplanar ? Detail(local.zy * scale + offset) * abs(N.x) + Detail(local.xz * scale + offset) * abs(N.y) +
                                  Detail(local.xy * scale + offset) * abs(N.z)
                            : Detail(local.xz * scale + offset);
        o.albedo = tint * lerp(0.75, 1.25, d);
        o.height = d;
    }
    o.normal = normal;
    o.roughness = (flags & F3D_LAYER_ROUGHNESS) != 0 ? nrm.b : g_LayerColor[layer].a;
    o.ao = (flags & F3D_LAYER_AO) != 0 ? nrm.a : 1.0;
    return o;
}

float4 main(PSInput input) : SV_Target
{
    float3 N = normalize(input.Normal);
    float3 local = input.WorldPos - g_TerrainOrigin.xyz;
    float2 uv = local.xz / g_TerrainParams.y + 0.5;
    uint layers = min(g_LayerInfo.x, F3D_TERRAIN_MAX_LAYERS);
    uint maps = g_LayerInfo.y;
    float distance = length(g_CameraPos.xyz - input.WorldPos);
    float2 dLocalX = ddx(local.xz);
    float2 dLocalY = ddy(local.xz);

    float weights[F3D_TERRAIN_MAX_LAYERS];
    float sum = 0.0;
    [unroll] for (uint m = 0; m < 4; ++m)
    {
        float4 w = m < maps ? g_TerrainSplat.Sample(g_TerrainSplat_sampler, float3(uv, m)) : 0.0;
        weights[m * 4 + 0] = w.r;
        weights[m * 4 + 1] = w.g;
        weights[m * 4 + 2] = w.b;
        weights[m * 4 + 3] = w.a;
        sum += w.r + w.g + w.b + w.a;
    }
    if (sum < 1e-3)
        weights[0] = 1.0; // nothing painted or baked: the base layer

    // The four strongest layers.
    uint top[4] = {0, 0, 0, 0};
    float topWeight[4] = {-1.0, -1.0, -1.0, -1.0};
    [loop] for (uint i = 0; i < layers; ++i)
    {
        float w = weights[i];
        if (w <= 0.002)
            continue;
        uint l = i;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            if (w > topWeight[k]) // insert, pushing the weaker ones down
            {
                float tw = topWeight[k];
                uint tl = top[k];
                topWeight[k] = w;
                top[k] = l;
                w = tw;
                l = tl;
            }
        }
    }

    // Height blending (Mishkinis, "Advanced Terrain Texture Splatting"): each layer's
    // weight plus its height scaled by its contrast; keep what is near the highest.
    LayerSample s[4];
    float score[4];
    float highest = -1.0;
    [unroll] for (uint a = 0; a < 4; ++a)
    {
        score[a] = -1.0;
        if (topWeight[a] > 0.0)
        {
            s[a] = SampleLayer(top[a], local, N, distance, dLocalX, dLocalY);
            score[a] = topWeight[a] + s[a].height * 0.5 * g_LayerParams2[top[a]].x;
            highest = max(highest, score[a]);
        }
        else
        {
            s[a] = (LayerSample)0;
        }
    }
    float3 albedo = 0.0;
    float3 normal = 0.0;
    float roughness = 0.0;
    float metallic = 0.0;
    float ao = 0.0;
    float total = 0.0;
    [unroll] for (uint b = 0; b < 4; ++b)
    {
        if (score[b] < 0.0)
            continue;
        float k = max(score[b] - highest + 0.2, 0.0);
        albedo += k * s[b].albedo;
        normal += k * s[b].normal;
        roughness += k * s[b].roughness;
        metallic += k * g_LayerParams[top[b]].z;
        ao += k * s[b].ao;
        total += k;
    }
    if (total > 0.0)
    {
        albedo /= total;
        roughness /= total;
        metallic /= total;
        ao /= total;
        N = normalize(normal);
    }
    else
    {
        albedo = g_LayerColor[0].rgb;
        roughness = g_LayerColor[0].a;
        ao = 1.0;
    }
    // Slow brightness variation over tens of metres breaks up repeats.
    albedo *= lerp(0.88, 1.12, ValueNoise(local.xz / 41.0) * 0.6 + ValueNoise(local.xz / 157.0 + 7.3) * 0.4);

    uint mode = ViewMode();
    if (mode == F3D_VIEW_UNLIT)
        return float4(albedo, 1.0);
    if (mode == F3D_VIEW_NORMALS)
        return float4(N * 0.5 + 0.5, 1.0);
    if (mode == F3D_VIEW_WIREFRAME)
        return float4(0.85, 0.87, 0.9, 1.0);

    Surface surf;
    surf.Position  = input.WorldPos;
    surf.Normal    = N;
    surf.View      = g_CameraPos.w > 0.5 ? normalize(g_View[2].xyz) : normalize(g_CameraPos.xyz - input.WorldPos);
    surf.Diffuse   = albedo * (1.0 - metallic);
    surf.F0        = lerp(0.04, albedo, metallic);
    surf.Roughness = saturate(roughness);
    float3 color = ShadeForward(surf, ao, 0, normalize(input.Normal), input.Pos.xy); // environment slot 0: the sky
    return FinishForward(color, input.WorldPos);
}
