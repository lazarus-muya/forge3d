// SPDX-License-Identifier: Apache-2.0
// Light clustering (phases.md P4.2): one thread per cluster tests every local
// light's bounding sphere against the cluster's view-space box and writes the
// cluster's light list. Same math as f3d::render::raster::assign_lights.
#include "lighting.hlsli"

StructuredBuffer<GpuLight> g_Lights;
RWStructuredBuffer<uint>   g_ClusterCounts;  // [F3D_CLUSTER_COUNT]
RWStructuredBuffer<uint>   g_ClusterIndices; // [F3D_CLUSTER_COUNT * F3D_MAX_LIGHTS_PER_CLUSTER]

float3 Unproject(float2 ndc, float z)
{
    float4 p = mul(g_InvProj, float4(ndc, z, 1.0));
    return p.xyz / p.w;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint cluster = id.x;
    if (cluster >= F3D_CLUSTER_COUNT)
        return;
    uint x = cluster % F3D_CLUSTER_TILES_X;
    uint y = (cluster / F3D_CLUSTER_TILES_X) % F3D_CLUSTER_TILES_Y;
    uint z = cluster / (F3D_CLUSTER_TILES_X * F3D_CLUSTER_TILES_Y);

    float d0 = exp(((float)z - g_ClusterParams.w) / g_ClusterParams.z);
    float d1 = exp(((float)z + 1.0 - g_ClusterParams.w) / g_ClusterParams.z);
    float3 boxMin = 1e30;
    float3 boxMax = -1e30;
    [unroll]
    for (uint corner = 0; corner < 4; ++corner)
    {
        float px  = (float)((x + (corner & 1)) * (uint)g_ClusterParams.x);
        float py  = (float)((y + ((corner >> 1) & 1)) * (uint)g_ClusterParams.y);
        float2 ndc = float2(2.0 * px * g_Viewport.z - 1.0, 1.0 - 2.0 * py * g_Viewport.w);
        float3 pn = Unproject(ndc, 0.0);
        float3 pf = Unproject(ndc, 1.0);
        float t0 = (d0 + pn.z) / (pn.z - pf.z);
        float t1 = (d1 + pn.z) / (pn.z - pf.z);
        float3 a = pn + t0 * (pf - pn);
        float3 b = pn + t1 * (pf - pn);
        boxMin = min(boxMin, min(a, b));
        boxMax = max(boxMax, max(a, b));
    }

    uint count = 0;
    for (uint i = 0; i < g_LightCounts.x && count < F3D_MAX_LIGHTS_PER_CLUSTER; ++i)
    {
        GpuLight light = g_Lights[i];
        float3 centre  = mul(g_View, float4(light.Position, 1.0)).xyz;
        float3 closest = clamp(centre, boxMin, boxMax);
        float3 d = closest - centre;
        if (dot(d, d) <= light.Range * light.Range)
        {
            g_ClusterIndices[cluster * F3D_MAX_LIGHTS_PER_CLUSTER + count] = i;
            ++count;
        }
    }
    g_ClusterCounts[cluster] = count;
}
