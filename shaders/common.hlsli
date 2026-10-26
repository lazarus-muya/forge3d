// SPDX-License-Identifier: Apache-2.0
// Shared definitions for all forge3d shaders.
#ifndef F3D_COMMON_HLSLI
#define F3D_COMMON_HLSLI

// Vertex input location. Inputs use semantics ATTRIB0..N (Diligent's convention);
// SPIR-V additionally needs explicit locations, which DXIL does not support.
#ifdef __spirv__
#define F3D_LOCATION(n) [[vk::location(n)]]
#else
#define F3D_LOCATION(n)
#endif

#endif // F3D_COMMON_HLSLI
