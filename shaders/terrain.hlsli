// SPDX-License-Identifier: Apache-2.0
// Terrain resources shared by terrain.vs and terrain.ps (phases.md P6.1, P6.4).
#ifndef F3D_TERRAIN_HLSLI
#define F3D_TERRAIN_HLSLI

#define F3D_TERRAIN_MAX_LODS 12

// f3d::render::raster::TerrainConstants.
cbuffer TerrainConstants
{
    float4x4 g_TerrainViewProj;              // the pass's view-projection (camera or shadow cascade)
    float4   g_TerrainOrigin;                // xyz: world position of terrain-local (0, 0, 0), w: sample spacing
    float4   g_TerrainParams;                // x: samples per side, y: size, z: height offset, w: height scale (R16) or 0
    float4   g_TerrainCamera;                // xyz: the camera, terrain-local (LOD distances)
    float4   g_TerrainDraw;                  // x: first node of this draw (SV_InstanceID starts at 0)
    float4   g_TerrainMorph[F3D_TERRAIN_MAX_LODS]; // per LOD: x morph start, y 1 / (morph end - start) (metres)
};

Texture2D<float>   g_TerrainHeight;
SamplerState       g_TerrainHeight_sampler;
StructuredBuffer<uint4> g_TerrainNodes; // per instance: first sample x, z, lod, quads

// Material layers (P6.4): f3d::render::raster::TerrainLayerConstants.
#define F3D_TERRAIN_MAX_LAYERS 16
#define F3D_LAYER_ALBEDO    1u // the layer's slices hold real textures (else the constants and procedural detail)
#define F3D_LAYER_NORMAL    2u
#define F3D_LAYER_ROUGHNESS 4u
#define F3D_LAYER_AO        8u
#define F3D_LAYER_HEIGHT    16u
cbuffer TerrainLayerConstants
{
    float4 g_LayerColor[F3D_TERRAIN_MAX_LAYERS];   // rgb: linear albedo or the albedo texture's tint, a: roughness
    float4 g_LayerParams[F3D_TERRAIN_MAX_LAYERS];  // x: 1 / tiling (per metre), y: triplanar, z: metallic, w: normal strength
    float4 g_LayerParams2[F3D_TERRAIN_MAX_LAYERS]; // x: height contrast, y: anti-tiling (0..1)
    uint4  g_LayerFlags[F3D_TERRAIN_MAX_LAYERS];   // x: F3D_LAYER_* flags
    uint4  g_LayerInfo;                            // x: layer count, y: splat maps
};

// One slice per layer. Albedo: sRGB-encoded colour, A height. Normal: R/G the tangent
// normal along +u/+v (scaled to 0..1), B roughness, A ambient occlusion.
Texture2DArray<float4> g_LayerAlbedo;
SamplerState           g_LayerAlbedo_sampler;
Texture2DArray<float4> g_LayerNormal;
SamplerState           g_LayerNormal_sampler;

// Layer weights: slice m holds layers 4m .. 4m+3 (RGBA8 UNORM, summing to 1 or all 0).
Texture2DArray<float4> g_TerrainSplat;
SamplerState           g_TerrainSplat_sampler;

#endif // F3D_TERRAIN_HLSLI
