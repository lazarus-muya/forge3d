// SPDX-License-Identifier: Apache-2.0
// Per-draw material constants (f3d::render::raster::MaterialConstants) and textures.
#ifndef F3D_MATERIAL_HLSLI
#define F3D_MATERIAL_HLSLI

#define F3D_MATERIAL_NORMAL_MAP      1u // the normal texture is real (else the vertex normal is used)
#define F3D_MATERIAL_ORM_OCCLUSION   2u // the ORM texture's red channel holds occlusion
#define F3D_MATERIAL_ALPHA_MASK      4u // discard below the alpha cutoff
#define F3D_MATERIAL_OCCLUSION_MAP   8u // a separate occlusion texture (red)

cbuffer MaterialConstants
{
    float4 g_BaseColorFactor;    // linear RGBA
    float4 g_EmissiveFactor;     // rgb: emissive x strength
    float  g_Metallic;
    float  g_Roughness;
    float  g_NormalScale;
    float  g_OcclusionStrength;
    float  g_AlphaCutoff;
    uint   g_MaterialFlags;
    float2 g_MaterialPad;
};

// Missing textures are bound to 1x1 white (normal: flat) defaults.
Texture2D    g_BaseColor;  // sRGB view: linear on sampling
SamplerState g_BaseColor_sampler;
Texture2D    g_Normal;
SamplerState g_Normal_sampler;
Texture2D    g_Orm;        // G roughness, B metallic (R occlusion if packed)
SamplerState g_Orm_sampler;
Texture2D    g_Occlusion;  // R occlusion
SamplerState g_Occlusion_sampler;
Texture2D    g_Emissive;   // sRGB view
SamplerState g_Emissive_sampler;

#endif // F3D_MATERIAL_HLSLI
