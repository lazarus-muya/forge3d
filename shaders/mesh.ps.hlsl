// SPDX-License-Identifier: Apache-2.0
// Clustered forward+ shading (phases.md P4.2, P4.3): metal/rough GGX + Lambert,
// every directional light (the first shadowed through cascaded shadow maps), the
// local lights of the pixel's cluster, and image-based lighting from the sky or the
// object's reflection probe (irradiance SH + prefiltered specular cube).
#include "forward.hlsli"
#include "material.hlsli"
#include "object.hlsli"

struct PSInput
{
    float4 Pos      : SV_Position;
    float3 Normal   : NORMAL;
    float3 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
};

float4 main(PSInput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    float4 baseColor = g_BaseColor.Sample(g_BaseColor_sampler, input.UV) * g_BaseColorFactor;
    baseColor.rgb *= input.Color;
    if ((g_MaterialFlags & F3D_MATERIAL_ALPHA_MASK) != 0 && baseColor.a < g_AlphaCutoff)
        discard;

    float3 N = normalize(input.Normal);
    if (!frontFace)
        N = -N; // double-sided materials: light the back face from its own side
    if ((g_MaterialFlags & F3D_MATERIAL_NORMAL_MAP) != 0)
    {
        float2 xy = g_Normal.Sample(g_Normal_sampler, input.UV).xy * 2.0 - 1.0;
        xy *= g_NormalScale;
        // Z reconstructed: works for BC5 (two channels) and full RGB normal maps.
        float3 tangentNormal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
        // glTF UVs have V down: the bitangent points along -V.
        float3x3 tbn = CotangentFrame(N, input.WorldPos, float2(input.UV.x, -input.UV.y));
        N = normalize(mul(tangentNormal, tbn));
    }

    float3 emissive = g_EmissiveFactor.rgb * g_Emissive.Sample(g_Emissive_sampler, input.UV).rgb;
    uint mode = ViewMode();
    if (mode == F3D_VIEW_UNLIT)
        return float4(baseColor.rgb + emissive, 1.0); // raw: the resolve doesn't tonemap
    if (mode == F3D_VIEW_NORMALS)
        return float4(N * 0.5 + 0.5, 1.0);
    if (mode == F3D_VIEW_WIREFRAME)
        return float4(0.85, 0.87, 0.9, 1.0);

    float4 orm = g_Orm.Sample(g_Orm_sampler, input.UV);
    float roughness = saturate(g_Roughness * orm.g);
    float metallic  = saturate(g_Metallic * orm.b);
    float occlusion = 1.0;
    if ((g_MaterialFlags & F3D_MATERIAL_ORM_OCCLUSION) != 0)
        occlusion = orm.r;
    else if ((g_MaterialFlags & F3D_MATERIAL_OCCLUSION_MAP) != 0)
        occlusion = g_Occlusion.Sample(g_Occlusion_sampler, input.UV).r;
    occlusion = lerp(1.0, occlusion, g_OcclusionStrength);

    Surface s;
    s.Position  = input.WorldPos;
    s.Normal    = N;
    s.View      = g_CameraPos.w > 0.5 ? normalize(g_View[2].xyz) // orthographic: the camera's +Z axis
                                      : normalize(g_CameraPos.xyz - input.WorldPos);
    s.Diffuse   = baseColor.rgb * (1.0 - metallic);
    s.F0        = lerp(0.04, baseColor.rgb, metallic);
    s.Roughness = roughness;

    float3 color = ShadeForward(s, occlusion, g_ObjectId.y, normalize(input.Normal), input.Pos.xy);
    color += emissive;
    return FinishForward(color, input.WorldPos);
}
