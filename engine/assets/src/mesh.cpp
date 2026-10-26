// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/mesh.hpp"

#include "f3d/core/debug_draw.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace f3d::assets {

MeshData make_uv_sphere(float radius, std::uint32_t segments, std::uint32_t rings) {
    MeshData mesh;
    mesh.name = "Sphere";
    segments = std::max(segments, 3u);
    rings = std::max(rings, 2u);
    constexpr float kPi = 3.14159265358979f;
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float theta = v * kPi; // from +Y down to -Y
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const glm::vec3 n(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi));
            mesh.vertices.push_back({n * radius, n, core::debug::kWhite});
            mesh.uvs.emplace_back(u, v);
            mesh.bounds.add(n * radius);
        }
    }
    const std::uint32_t stride = segments + 1;
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < segments; ++s) {
            const std::uint32_t a = r * stride + s;
            const std::uint32_t b = a + stride;
            // Counter-clockwise seen from outside.
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    mesh.primitive_count = 1;
    return mesh;
}

std::vector<SubMesh> submeshes_of(const MeshData& mesh) {
    if (!mesh.submeshes.empty()) {
        return mesh.submeshes;
    }
    if (mesh.indices.empty()) {
        return {};
    }
    return {SubMesh{0, static_cast<std::uint32_t>(mesh.indices.size()), SubMesh::kNoMaterial}};
}

std::vector<SubMesh> group_by_material(MeshData& mesh, const std::vector<std::uint32_t>& triangle_material) {
    const std::size_t triangles = mesh.indices.size() / 3;
    if (triangle_material.size() != triangles || triangles == 0) {
        return {};
    }
    std::vector<std::uint32_t> order(triangles);
    std::iota(order.begin(), order.end(), 0u);
    // kNoMaterial sorts last, so material ranges come in material order.
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return triangle_material[a] < triangle_material[b]; });
    std::vector<std::uint32_t> indices(mesh.indices.size());
    std::vector<SubMesh> submeshes;
    for (std::size_t t = 0; t < triangles; ++t) {
        const std::uint32_t source = order[t];
        std::copy_n(mesh.indices.begin() + static_cast<std::ptrdiff_t>(source) * 3, 3, indices.begin() + t * 3);
        const std::uint32_t material = triangle_material[source];
        if (submeshes.empty() || submeshes.back().material != material) {
            submeshes.push_back({static_cast<std::uint32_t>(t * 3), 0, material});
        }
        submeshes.back().index_count += 3;
    }
    mesh.indices = std::move(indices);
    // A single range without a material carries no information.
    if (submeshes.size() == 1 && submeshes[0].material == SubMesh::kNoMaterial) {
        submeshes.clear();
    }
    return submeshes;
}

} // namespace f3d::assets
