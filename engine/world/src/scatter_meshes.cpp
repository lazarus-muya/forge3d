// SPDX-License-Identifier: Apache-2.0
// Built-in scatter meshes: low-poly, vertex-coloured placeholders (scatter.hpp).

#include "f3d/core/debug_draw.hpp"
#include "f3d/world/scatter.hpp"

#include <cmath>

namespace f3d::world {
namespace {

using core::debug::rgba8;
constexpr float kPi = 3.14159265f;

void add_vertex(assets::MeshData& mesh, const glm::vec3& p, const glm::vec3& n, std::uint32_t color) {
    mesh.vertices.push_back({p, n, color});
    mesh.bounds.add(p);
}

std::uint32_t base_index(const assets::MeshData& mesh) {
    return static_cast<std::uint32_t>(mesh.vertices.size());
}

/// A truncated cone along +Y (a cylinder when the radii match, a cone at top = 0),
/// with a bottom cap.
void add_cone(assets::MeshData& mesh, float bottom, float top, float r0, float r1, std::uint32_t segments,
              std::uint32_t color) {
    const float slant = (r0 - r1) / (top - bottom); // normal tilt towards +Y
    const std::uint32_t first = base_index(mesh);
    for (std::uint32_t s = 0; s <= segments; ++s) {
        const float a = static_cast<float>(s) / static_cast<float>(segments) * 2.0f * kPi;
        const glm::vec3 radial(std::sin(a), 0.0f, std::cos(a));
        const glm::vec3 n = glm::normalize(radial + glm::vec3(0.0f, slant, 0.0f));
        add_vertex(mesh, radial * r0 + glm::vec3(0.0f, bottom, 0.0f), n, color);
        add_vertex(mesh, radial * r1 + glm::vec3(0.0f, top, 0.0f), n, color);
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        const std::uint32_t a = first + s * 2;
        // Counter-clockwise seen from outside.
        mesh.indices.insert(mesh.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
    }
    const std::uint32_t centre = base_index(mesh);
    add_vertex(mesh, glm::vec3(0.0f, bottom, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), color);
    for (std::uint32_t s = 0; s <= segments; ++s) {
        const float a = static_cast<float>(s) / static_cast<float>(segments) * 2.0f * kPi;
        add_vertex(mesh, glm::vec3(std::sin(a) * r0, bottom, std::cos(a) * r0), glm::vec3(0.0f, -1.0f, 0.0f), color);
    }
    for (std::uint32_t s = 0; s < segments; ++s) {
        mesh.indices.insert(mesh.indices.end(), {centre, centre + 2 + s, centre + 1 + s});
    }
}

/// A lumpy blob: a UV sphere scaled by `radii`, displaced by up to `bumps` (fraction
/// of the radius) with a hash of the direction (so seam vertices move together).
void add_blob(assets::MeshData& mesh, const glm::vec3& centre, const glm::vec3& radii, float bumps,
              std::uint32_t seed, std::uint32_t segments, std::uint32_t rings, std::uint32_t color) {
    const assets::MeshData sphere = assets::make_uv_sphere(1.0f, segments, rings);
    const std::uint32_t first = base_index(mesh);
    const std::size_t first_index = mesh.indices.size();
    for (const assets::MeshVertex& v : sphere.vertices) {
        const glm::ivec3 q = glm::ivec3(glm::round(v.position * 64.0f));
        std::uint32_t h = seed * 0x9E3779B9u ^ static_cast<std::uint32_t>(q.x * 73856093 ^ q.y * 19349663 ^ q.z * 83492791);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        const float bump = 1.0f + bumps * (static_cast<float>(h & 0xFFFFu) / 65535.0f * 2.0f - 1.0f);
        add_vertex(mesh, centre + v.position * radii * bump, glm::vec3(0.0f, 1.0f, 0.0f), color);
    }
    for (const std::uint32_t i : sphere.indices) {
        mesh.indices.push_back(first + i);
    }
    assets::compute_normals(mesh, first, first_index);
}

/// A thin blade from the origin leaning towards `lean`, both faces (meshes draw back-face culled).
void add_blade(assets::MeshData& mesh, float angle, float height, float width, const glm::vec3& lean,
               std::uint32_t root, std::uint32_t tip) {
    const glm::vec3 side(std::cos(angle) * width * 0.5f, 0.0f, std::sin(angle) * width * 0.5f);
    const glm::vec3 top = glm::vec3(0.0f, height, 0.0f) + lean;
    const glm::vec3 n = glm::normalize(glm::cross(side, top));
    for (const float facing : {1.0f, -1.0f}) {
        const std::uint32_t first = base_index(mesh);
        add_vertex(mesh, -side, n * facing, root);
        add_vertex(mesh, side, n * facing, root);
        add_vertex(mesh, top, n * facing, tip);
        if (facing > 0.0f) {
            mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2});
        } else {
            mesh.indices.insert(mesh.indices.end(), {first, first + 2, first + 1});
        }
    }
}

assets::MeshData finish(assets::MeshData mesh, const char* name) {
    mesh.name = name;
    mesh.uvs.assign(mesh.vertices.size(), glm::vec2(0.0f));
    mesh.primitive_count = 1;
    return mesh;
}

assets::MeshData make_pine() {
    assets::MeshData mesh;
    const std::uint32_t bark = rgba8(92, 64, 42);
    const std::uint32_t needles = rgba8(38, 82, 44);
    add_cone(mesh, -0.5f, 2.0f, 0.22f, 0.14f, 8, bark);
    add_cone(mesh, 1.2f, 4.2f, 1.9f, 0.0f, 10, needles);
    add_cone(mesh, 3.0f, 5.9f, 1.45f, 0.0f, 10, needles);
    add_cone(mesh, 4.7f, 7.6f, 1.0f, 0.0f, 10, needles);
    return finish(std::move(mesh), "Pine");
}

assets::MeshData make_broadleaf() {
    assets::MeshData mesh;
    add_cone(mesh, -0.5f, 3.0f, 0.3f, 0.2f, 8, rgba8(98, 74, 52));
    add_blob(mesh, {0.0f, 4.6f, 0.0f}, {2.4f, 2.0f, 2.4f}, 0.18f, 1, 12, 8, rgba8(66, 110, 46));
    add_blob(mesh, {0.9f, 5.6f, -0.4f}, {1.4f, 1.2f, 1.4f}, 0.15f, 2, 10, 6, rgba8(74, 120, 50));
    return finish(std::move(mesh), "Broadleaf");
}

assets::MeshData make_grass() {
    assets::MeshData mesh;
    const std::uint32_t root = rgba8(46, 74, 26);
    const std::uint32_t tip = rgba8(132, 168, 72);
    constexpr int kBlades = 9;
    for (int i = 0; i < kBlades; ++i) {
        const float t = static_cast<float>(i) / kBlades;
        const float angle = t * 2.0f * kPi * 1.618f;
        const glm::vec3 offset(std::cos(angle * 2.3f) * 0.08f, 0.0f, std::sin(angle * 2.3f) * 0.08f);
        const float height = 0.35f + 0.25f * std::fmod(t * 7.0f, 1.0f);
        const glm::vec3 lean = glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * 0.12f;
        const std::uint32_t first = base_index(mesh);
        add_blade(mesh, angle + kPi * 0.5f, height, 0.05f, lean, root, tip);
        for (std::uint32_t v = first; v < base_index(mesh); ++v) {
            mesh.vertices[v].position += offset;
            mesh.bounds.add(mesh.vertices[v].position);
        }
    }
    return finish(std::move(mesh), "Grass");
}

assets::MeshData make_rock() {
    assets::MeshData mesh;
    add_blob(mesh, {0.0f, 0.15f, 0.0f}, {0.6f, 0.4f, 0.5f}, 0.22f, 7, 10, 7, rgba8(118, 112, 104));
    return finish(std::move(mesh), "Rock");
}

} // namespace

std::vector<std::pair<std::string, assets::MeshData>> builtin_scatter_meshes() {
    std::vector<std::pair<std::string, assets::MeshData>> out;
    out.emplace_back(kBuiltinPine, make_pine());
    out.emplace_back(kBuiltinBroadleaf, make_broadleaf());
    out.emplace_back(kBuiltinGrass, make_grass());
    out.emplace_back(kBuiltinRock, make_rock());
    return out;
}

} // namespace f3d::world
