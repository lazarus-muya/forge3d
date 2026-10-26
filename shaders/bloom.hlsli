// SPDX-License-Identifier: Apache-2.0
// Bloom passes (f3d::render::raster::BloomConstants): a chain of half-sized levels,
// downsampled from the scene colour, then upsampled and summed back up.
#ifndef F3D_BLOOM_HLSLI
#define F3D_BLOOM_HLSLI

Texture2D<float4> g_Source;
SamplerState      g_Source_sampler;

cbuffer BloomConstants
{
    float4 g_BloomSource; // xy: source texel size (uv), z: 1 = first downsample (from the scene), w: threshold luminance (0: none)
    float4 g_BloomTarget; // xy: target texel size (uv), z: upsampling filter radius (source texels)
};

struct PSInput
{
    float4 Pos : SV_Position;
    float2 Ndc : TEXCOORD0;
};

#endif // F3D_BLOOM_HLSLI
