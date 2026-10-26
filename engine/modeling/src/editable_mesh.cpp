// SPDX-License-Identifier: Apache-2.0
#include "f3d/modeling/editable_mesh.hpp"

#include "f3d/assets/mesh_library.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace f3d::modeling {
namespace {

using Faces = std::vector<std::uint32_t>;

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    return (static_cast<std::uint64_t>(a) << 32) | b;
}

std::uint64_t undirected_key(std::uint32_t a, std::uint32_t b) {
    return a < b ? edge_key(a, b) : edge_key(b, a);
}

/// Newell's (unnormalised) normal of a polygon: its length is twice the area.
glm::vec3 newell(const EditableMesh& mesh, std::uint32_t offset, std::uint32_t size) {
    glm::vec3 n(0.0f);
    for (std::uint32_t i = 0; i < size; ++i) {
        const glm::vec3& a = mesh.positions[mesh.corners[offset + i]];
        const glm::vec3& b = mesh.positions[mesh.corners[offset + (i + 1) % size]];
        n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return n;
}

/// Removes positions no face uses (keeps the order of the rest).
void compact(EditableMesh& mesh) {
    std::vector<std::uint32_t> remap(mesh.positions.size(), ~0u);
    for (const std::uint32_t c : mesh.corners) {
        remap[c] = 0;
    }
    std::vector<glm::vec3> positions;
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        if (remap[i] == 0) {
            remap[i] = static_cast<std::uint32_t>(positions.size());
            positions.push_back(mesh.positions[i]);
        }
    }
    for (std::uint32_t& c : mesh.corners) {
        c = remap[c];
    }
    mesh.positions = std::move(positions);
}

/// The valid, distinct face indices of `faces` (sorted).
Faces selection(const EditableMesh& mesh, const Faces& faces) {
    Faces out;
    for (const std::uint32_t f : faces) {
        if (f < mesh.face_sizes.size()) {
            out.push_back(f);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// Rebuilds face_sizes/corners from a list of polygons.
void set_faces(EditableMesh& mesh, const std::vector<std::vector<std::uint32_t>>& faces) {
    mesh.face_sizes.clear();
    mesh.corners.clear();
    for (const auto& f : faces) {
        mesh.face_sizes.push_back(static_cast<std::uint32_t>(f.size()));
        mesh.corners.insert(mesh.corners.end(), f.begin(), f.end());
    }
}

std::vector<std::vector<std::uint32_t>> polygons(const EditableMesh& mesh) {
    std::vector<std::vector<std::uint32_t>> out;
    std::uint32_t offset = 0;
    for (const std::uint32_t size : mesh.face_sizes) {
        out.emplace_back(mesh.corners.begin() + offset, mesh.corners.begin() + offset + size);
        offset += size;
    }
    return out;
}

/// Triangles of one face as corner indices (offsets into mesh.corners): a fan for
/// convex faces, ear clipping (in the face's plane) otherwise.
void face_triangles(const EditableMesh& mesh, std::uint32_t offset, std::uint32_t size,
                    std::vector<std::uint32_t>& out) {
    if (size == 3) {
        out.insert(out.end(), {offset, offset + 1, offset + 2});
        return;
    }
    const glm::vec3 normal = newell(mesh, offset, size);
    const float length = glm::length(normal);
    const glm::vec3 n = length > 0.0f ? normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 axis = std::abs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 u = glm::normalize(glm::cross(axis, n));
    const glm::vec3 v = glm::cross(n, u);
    std::vector<glm::vec2> p(size);
    for (std::uint32_t i = 0; i < size; ++i) {
        const glm::vec3& q = mesh.positions[mesh.corners[offset + i]];
        p[i] = glm::vec2(glm::dot(q, u), glm::dot(q, v));
    }
    const auto cross2 = [](const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; };
    bool convex = true;
    for (std::uint32_t i = 0; i < size && convex; ++i) {
        convex = cross2(p[(i + 1) % size] - p[i], p[(i + 2) % size] - p[(i + 1) % size]) >= -1e-9f;
    }
    if (convex) {
        for (std::uint32_t i = 1; i + 1 < size; ++i) {
            out.insert(out.end(), {offset, offset + i, offset + i + 1});
        }
        return;
    }
    std::vector<std::uint32_t> remaining(size);
    std::iota(remaining.begin(), remaining.end(), 0u);
    const auto inside = [&](const glm::vec2& q, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
        return cross2(b - a, q - a) > 0.0f && cross2(c - b, q - b) > 0.0f && cross2(a - c, q - c) > 0.0f;
    };
    std::size_t guard = 0;
    while (remaining.size() > 3 && guard++ < size * size) {
        bool clipped = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const std::uint32_t ia = remaining[(i + remaining.size() - 1) % remaining.size()];
            const std::uint32_t ib = remaining[i];
            const std::uint32_t ic = remaining[(i + 1) % remaining.size()];
            if (cross2(p[ib] - p[ia], p[ic] - p[ib]) <= 0.0f) {
                continue; // reflex
            }
            bool ear = true;
            for (const std::uint32_t k : remaining) {
                if (k != ia && k != ib && k != ic && inside(p[k], p[ia], p[ib], p[ic])) {
                    ear = false;
                    break;
                }
            }
            if (ear) {
                out.insert(out.end(), {offset + ia, offset + ib, offset + ic});
                remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
                clipped = true;
                break;
            }
        }
        if (!clipped) {
            break; // degenerate: fan the rest
        }
    }
    for (std::size_t i = 1; i + 1 < remaining.size(); ++i) {
        out.insert(out.end(), {offset + remaining[0], offset + remaining[i], offset + remaining[i + 1]});
    }
}

std::uint64_t hash_mesh(const EditableMesh& mesh) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* data, std::size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            h = (h ^ p[i]) * 1099511628211ull;
        }
    };
    mix(mesh.positions.data(), mesh.positions.size() * sizeof(glm::vec3));
    mix(mesh.face_sizes.data(), mesh.face_sizes.size() * sizeof(std::uint32_t));
    mix(mesh.corners.data(), mesh.corners.size() * sizeof(std::uint32_t));
    mix(&mesh.smoothing_angle, sizeof(float));
    mix(&mesh.uv_scale, sizeof(float));
    return h;
}

/// Per-registry cache of what sync_meshes() uploaded: (library, entity) -> mesh hash.
struct SyncCache {
    std::map<std::pair<const assets::MeshLibrary*, core::Guid>, std::uint64_t> hashes;
};

} // namespace

std::vector<std::uint32_t> face_offsets(const EditableMesh& mesh) {
    std::vector<std::uint32_t> out(mesh.face_sizes.size());
    std::uint32_t offset = 0;
    for (std::size_t f = 0; f < mesh.face_sizes.size(); ++f) {
        out[f] = offset;
        offset += mesh.face_sizes[f];
    }
    return out;
}

bool is_valid(const EditableMesh& mesh) noexcept {
    std::size_t total = 0;
    for (const std::uint32_t size : mesh.face_sizes) {
        if (size < 3) {
            return false;
        }
        total += size;
    }
    if (total != mesh.corners.size()) {
        return false;
    }
    return std::all_of(mesh.corners.begin(), mesh.corners.end(),
                       [&](std::uint32_t c) { return c < mesh.positions.size(); });
}

glm::vec3 face_normal(const EditableMesh& mesh, std::uint32_t face) {
    std::uint32_t offset = 0;
    for (std::uint32_t f = 0; f < face; ++f) {
        offset += mesh.face_sizes[f];
    }
    const glm::vec3 n = newell(mesh, offset, mesh.face_sizes[face]);
    const float length = glm::length(n);
    return length > 0.0f ? n / length : glm::vec3(0.0f);
}

glm::vec3 face_centre(const EditableMesh& mesh, std::uint32_t face) {
    std::uint32_t offset = 0;
    for (std::uint32_t f = 0; f < face; ++f) {
        offset += mesh.face_sizes[f];
    }
    glm::vec3 c(0.0f);
    for (std::uint32_t i = 0; i < mesh.face_sizes[face]; ++i) {
        c += mesh.positions[mesh.corners[offset + i]];
    }
    return c / static_cast<float>(mesh.face_sizes[face]);
}

assets::MeshData triangulate(const EditableMesh& mesh, std::string_view name) {
    F3D_ZONE;
    assets::MeshData out;
    out.name = name.empty() ? std::string("Editable mesh") : std::string(name);
    out.primitive_count = 1;
    if (!is_valid(mesh)) {
        return out;
    }
    const std::size_t faces = mesh.face_sizes.size();
    const std::vector<std::uint32_t> offsets = face_offsets(mesh);
    std::vector<glm::vec3> area_normals(faces);
    std::vector<glm::vec3> normals(faces);
    for (std::size_t f = 0; f < faces; ++f) {
        area_normals[f] = newell(mesh, offsets[f], mesh.face_sizes[f]);
        const float length = glm::length(area_normals[f]);
        normals[f] = length > 0.0f ? area_normals[f] / length : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    // Faces around each position, for smoothing.
    std::vector<std::vector<std::uint32_t>> around(mesh.positions.size());
    if (mesh.smoothing_angle > 0.0f) {
        for (std::size_t f = 0; f < faces; ++f) {
            for (std::uint32_t i = 0; i < mesh.face_sizes[f]; ++i) {
                around[mesh.corners[offsets[f] + i]].push_back(static_cast<std::uint32_t>(f));
            }
        }
    }
    const float threshold = std::cos(glm::radians(std::clamp(mesh.smoothing_angle, 0.0f, 180.0f))) - 1e-4f;
    const float uv_scale = 1.0f / std::max(mesh.uv_scale, 1e-4f);
    out.vertices.reserve(mesh.corners.size());
    out.uvs.reserve(mesh.corners.size());
    std::vector<std::uint32_t> triangles;
    for (std::size_t f = 0; f < faces; ++f) {
        const glm::vec3& n = normals[f];
        const glm::vec3 a = glm::abs(n);
        const int axis = a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
        for (std::uint32_t i = 0; i < mesh.face_sizes[f]; ++i) {
            const std::uint32_t position = mesh.corners[offsets[f] + i];
            const glm::vec3& p = mesh.positions[position];
            glm::vec3 shading = n;
            if (mesh.smoothing_angle > 0.0f) {
                glm::vec3 sum(0.0f);
                for (const std::uint32_t g : around[position]) {
                    if (glm::dot(normals[g], n) >= threshold) {
                        sum += area_normals[g];
                    }
                }
                const float length = glm::length(sum);
                shading = length > 0.0f ? sum / length : n;
            }
            out.vertices.push_back({p, shading, 0xFFFFFFFFu});
            out.bounds.add(p);
            // Box projection, mirrored on the negative sides so textures read the right way.
            glm::vec2 uv = axis == 0 ? glm::vec2(n.x > 0.0f ? -p.z : p.z, -p.y)
                         : axis == 1 ? glm::vec2(p.x, n.y > 0.0f ? p.z : -p.z)
                                     : glm::vec2(n.z > 0.0f ? p.x : -p.x, -p.y);
            out.uvs.push_back(uv * uv_scale);
        }
        triangles.clear();
        face_triangles(mesh, offsets[f], mesh.face_sizes[f], triangles);
        out.indices.insert(out.indices.end(), triangles.begin(), triangles.end());
    }
    return out;
}

std::optional<FaceHit> raycast(const EditableMesh& mesh, const glm::vec3& origin, const glm::vec3& direction) {
    if (!is_valid(mesh)) {
        return std::nullopt;
    }
    std::optional<FaceHit> best;
    const std::vector<std::uint32_t> offsets = face_offsets(mesh);
    std::vector<std::uint32_t> triangles;
    for (std::size_t f = 0; f < mesh.face_sizes.size(); ++f) {
        triangles.clear();
        face_triangles(mesh, offsets[f], mesh.face_sizes[f], triangles);
        for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
            const glm::vec3& a = mesh.positions[mesh.corners[triangles[t]]];
            const glm::vec3& b = mesh.positions[mesh.corners[triangles[t + 1]]];
            const glm::vec3& c = mesh.positions[mesh.corners[triangles[t + 2]]];
            // Moller-Trumbore, both sides.
            const glm::vec3 e1 = b - a, e2 = c - a;
            const glm::vec3 p = glm::cross(direction, e2);
            const float det = glm::dot(e1, p);
            if (std::abs(det) < 1e-12f) {
                continue;
            }
            const float inv = 1.0f / det;
            const glm::vec3 s = origin - a;
            const float u = glm::dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            const glm::vec3 q = glm::cross(s, e1);
            const float v = glm::dot(direction, q) * inv;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            const float distance = glm::dot(e2, q) * inv;
            if (distance > 0.0f && (!best || distance < best->distance)) {
                best = FaceHit{static_cast<std::uint32_t>(f), distance};
            }
        }
    }
    return best;
}

EditableMesh extrude_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces, float distance) {
    EditableMesh out = mesh;
    const Faces selected = selection(mesh, faces);
    if (selected.empty() || !is_valid(mesh)) {
        return out;
    }
    auto polys = polygons(mesh);
    // Directed edges of the region, to find its boundary.
    std::unordered_set<std::uint64_t> region_edges;
    std::unordered_map<std::uint32_t, glm::vec3> vertex_normals;
    for (const std::uint32_t f : selected) {
        const auto& poly = polys[f];
        const glm::vec3 n = face_normal(mesh, f);
        for (std::size_t i = 0; i < poly.size(); ++i) {
            region_edges.insert(edge_key(poly[i], poly[(i + 1) % poly.size()]));
            vertex_normals[poly[i]] += n;
        }
    }
    // A moved copy of every position of the region.
    std::unordered_map<std::uint32_t, std::uint32_t> moved;
    for (const auto& [position, normal] : vertex_normals) {
        const float length = glm::length(normal);
        const glm::vec3 dir = length > 0.0f ? normal / length : glm::vec3(0.0f);
        moved[position] = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(mesh.positions[position] + dir * distance);
    }
    std::vector<std::vector<std::uint32_t>> sides;
    for (const std::uint32_t f : selected) {
        auto& poly = polys[f];
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const std::uint32_t a = poly[i];
            const std::uint32_t b = poly[(i + 1) % poly.size()];
            if (!region_edges.contains(edge_key(b, a))) { // a boundary edge: a side wall
                sides.push_back({a, b, moved[b], moved[a]});
            }
        }
        for (std::uint32_t& c : poly) {
            c = moved[c];
        }
    }
    polys.insert(polys.end(), sides.begin(), sides.end());
    set_faces(out, polys);
    compact(out);
    return out;
}

EditableMesh inset_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces, float amount) {
    EditableMesh out = mesh;
    const Faces selected = selection(mesh, faces);
    if (selected.empty() || !is_valid(mesh)) {
        return out;
    }
    const float t = std::clamp(amount, 0.0f, 0.999f);
    auto polys = polygons(mesh);
    std::vector<std::vector<std::uint32_t>> rings;
    for (const std::uint32_t f : selected) {
        auto& poly = polys[f];
        const glm::vec3 centre = face_centre(mesh, f);
        std::vector<std::uint32_t> inner;
        for (const std::uint32_t c : poly) {
            inner.push_back(static_cast<std::uint32_t>(out.positions.size()));
            out.positions.push_back(glm::mix(mesh.positions[c], centre, t));
        }
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const std::size_t j = (i + 1) % poly.size();
            rings.push_back({poly[i], poly[j], inner[j], inner[i]});
        }
        poly = inner;
    }
    polys.insert(polys.end(), rings.begin(), rings.end());
    set_faces(out, polys);
    return out;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> edge_ring(const EditableMesh& mesh, std::uint32_t a,
                                                                std::uint32_t b) {
    using Edge = std::pair<std::uint32_t, std::uint32_t>;
    std::vector<Edge> ring;
    if (!is_valid(mesh) || a == b) {
        return ring;
    }
    const auto polys = polygons(mesh);
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edge_faces;
    for (std::size_t f = 0; f < polys.size(); ++f) {
        const auto& poly = polys[f];
        for (std::size_t i = 0; i < poly.size(); ++i) {
            edge_faces[undirected_key(poly[i], poly[(i + 1) % poly.size()])].push_back(static_cast<std::uint32_t>(f));
        }
    }
    const auto start = edge_faces.find(undirected_key(a, b));
    if (start == edge_faces.end()) {
        return ring;
    }
    ring.emplace_back(a, b);
    std::unordered_set<std::uint64_t> seen{undirected_key(a, b)};
    std::unordered_set<std::uint32_t> crossed;
    // Walk across the quads on each side of the start edge.
    std::vector<Edge> backwards;
    for (std::size_t side = 0; side < start->second.size() && side < 2; ++side) {
        std::vector<Edge>& out = side == 0 ? ring : backwards;
        Edge edge{a, b};
        std::uint32_t face = start->second[side];
        while (crossed.insert(face).second) {
            const auto& q = polys[face];
            if (q.size() != 4) {
                break;
            }
            std::size_t i = 0;
            while (i < 4 && undirected_key(q[i], q[(i + 1) % 4]) != undirected_key(edge.first, edge.second)) {
                ++i;
            }
            if (i == 4) {
                break;
            }
            // q[i] is next to q[i+3], q[i+1] next to q[i+2]: the opposite edge, oriented alike.
            const Edge next = edge.first == q[i] ? Edge{q[(i + 3) % 4], q[(i + 2) % 4]}
                                                 : Edge{q[(i + 2) % 4], q[(i + 3) % 4]};
            if (!seen.insert(undirected_key(next.first, next.second)).second) {
                break; // a closed ring
            }
            out.push_back(next);
            edge = next;
            const auto& faces = edge_faces[undirected_key(next.first, next.second)];
            const auto other = std::find_if(faces.begin(), faces.end(), [&](std::uint32_t f) { return f != face; });
            if (other == faces.end()) {
                break; // an open edge
            }
            face = *other;
        }
    }
    ring.insert(ring.end(), backwards.begin(), backwards.end());
    return ring;
}

EditableMesh loop_cut(const EditableMesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t cuts, float factor) {
    EditableMesh out = mesh;
    const auto ring = edge_ring(mesh, a, b);
    if (ring.empty() || cuts == 0) {
        return out;
    }
    cuts = std::min(cuts, 64u);
    // New positions along every ring edge, ordered from its first end.
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::vector<std::uint32_t>>> points;
    for (const auto& [from, to] : ring) {
        std::vector<std::uint32_t> added;
        for (std::uint32_t k = 1; k <= cuts; ++k) {
            const float t = cuts == 1 ? std::clamp(factor, 0.0f, 1.0f) : static_cast<float>(k) / (cuts + 1.0f);
            added.push_back(static_cast<std::uint32_t>(out.positions.size()));
            out.positions.push_back(glm::mix(mesh.positions[from], mesh.positions[to], t));
        }
        points[undirected_key(from, to)] = {from, std::move(added)};
    }
    // The positions inserted between x and y (in that order), or null if (x, y) is not cut.
    const auto between = [&](std::uint32_t x, std::uint32_t y) -> std::optional<std::vector<std::uint32_t>> {
        const auto it = points.find(undirected_key(x, y));
        if (it == points.end()) {
            return std::nullopt;
        }
        std::vector<std::uint32_t> p = it->second.second;
        if (it->second.first != x) {
            std::reverse(p.begin(), p.end());
        }
        return p;
    };
    std::vector<std::vector<std::uint32_t>> result;
    for (const auto& poly : polygons(mesh)) {
        // A quad with two opposite cut edges is split into strips.
        bool split = false;
        if (poly.size() == 4) {
            for (std::size_t i = 0; i < 2 && !split; ++i) {
                const auto first = between(poly[i], poly[i + 1]);
                const auto second = between(poly[(i + 3) % 4], poly[i + 2]);
                if (!first || !second) {
                    continue;
                }
                std::vector<std::uint32_t> left{poly[i]}, right{poly[(i + 3) % 4]};
                left.insert(left.end(), first->begin(), first->end());
                right.insert(right.end(), second->begin(), second->end());
                left.push_back(poly[i + 1]);
                right.push_back(poly[i + 2]);
                for (std::size_t k = 0; k + 1 < left.size(); ++k) {
                    result.push_back({left[k], left[k + 1], right[k + 1], right[k]});
                }
                split = true;
            }
        }
        if (split) {
            continue;
        }
        // Any other face keeps its shape, with the new positions on its cut edges.
        std::vector<std::uint32_t> outline;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            outline.push_back(poly[i]);
            if (const auto inserted = between(poly[i], poly[(i + 1) % poly.size()])) {
                outline.insert(outline.end(), inserted->begin(), inserted->end());
            }
        }
        result.push_back(std::move(outline));
    }
    set_faces(out, result);
    return out;
}

EditableMesh delete_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces) {
    EditableMesh out = mesh;
    const Faces selected = selection(mesh, faces);
    if (selected.empty() || !is_valid(mesh)) {
        return out;
    }
    auto polys = polygons(mesh);
    std::vector<std::vector<std::uint32_t>> kept;
    for (std::size_t f = 0; f < polys.size(); ++f) {
        if (!std::binary_search(selected.begin(), selected.end(), static_cast<std::uint32_t>(f))) {
            kept.push_back(std::move(polys[f]));
        }
    }
    set_faces(out, kept);
    compact(out);
    return out;
}

EditableMesh flip_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces) {
    EditableMesh out = mesh;
    if (!is_valid(mesh)) {
        return out;
    }
    Faces selected = selection(mesh, faces);
    if (faces.empty()) {
        selected.resize(mesh.face_sizes.size());
        std::iota(selected.begin(), selected.end(), 0u);
    }
    const std::vector<std::uint32_t> offsets = face_offsets(mesh);
    for (const std::uint32_t f : selected) {
        std::reverse(out.corners.begin() + offsets[f], out.corners.begin() + offsets[f] + mesh.face_sizes[f]);
    }
    return out;
}

EditableMesh subdivide(const EditableMesh& mesh, std::uint32_t levels) {
    EditableMesh current = mesh;
    for (std::uint32_t level = 0; level < std::min(levels, 4u) && is_valid(current); ++level) {
        const auto polys = polygons(current);
        const std::size_t nv = current.positions.size();
        // Face points.
        std::vector<glm::vec3> face_points(polys.size());
        for (std::size_t f = 0; f < polys.size(); ++f) {
            glm::vec3 c(0.0f);
            for (const std::uint32_t v : polys[f]) {
                c += current.positions[v];
            }
            face_points[f] = c / static_cast<float>(polys[f].size());
        }
        // Edges with their faces.
        std::unordered_map<std::uint64_t, std::uint32_t> edge_ids;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
        std::vector<std::vector<std::uint32_t>> edge_faces;
        for (std::size_t f = 0; f < polys.size(); ++f) {
            const auto& poly = polys[f];
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const std::uint32_t a = poly[i], b = poly[(i + 1) % poly.size()];
                const auto [it, inserted] =
                    edge_ids.try_emplace(undirected_key(a, b), static_cast<std::uint32_t>(edges.size()));
                if (inserted) {
                    edges.emplace_back(a, b);
                    edge_faces.emplace_back();
                }
                edge_faces[it->second].push_back(static_cast<std::uint32_t>(f));
            }
        }
        std::vector<glm::vec3> edge_points(edges.size());
        std::vector<bool> boundary_vertex(nv, false);
        std::vector<glm::vec3> boundary_sum(nv, glm::vec3(0.0f));
        std::vector<glm::vec3> edge_mid_sum(nv, glm::vec3(0.0f));
        std::vector<std::uint32_t> edge_count(nv, 0);
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const auto [a, b] = edges[e];
            const glm::vec3 mid = (current.positions[a] + current.positions[b]) * 0.5f;
            if (edge_faces[e].size() == 1) {
                edge_points[e] = mid;
                boundary_vertex[a] = boundary_vertex[b] = true;
                boundary_sum[a] += current.positions[b];
                boundary_sum[b] += current.positions[a];
            } else {
                glm::vec3 faces_avg(0.0f);
                for (const std::uint32_t f : edge_faces[e]) {
                    faces_avg += face_points[f];
                }
                faces_avg /= static_cast<float>(edge_faces[e].size());
                edge_points[e] = (mid + faces_avg) * 0.5f;
            }
            edge_mid_sum[a] += mid;
            edge_mid_sum[b] += mid;
            ++edge_count[a];
            ++edge_count[b];
        }
        // Vertex points.
        std::vector<glm::vec3> face_sum(nv, glm::vec3(0.0f));
        std::vector<std::uint32_t> face_count(nv, 0);
        for (std::size_t f = 0; f < polys.size(); ++f) {
            for (const std::uint32_t v : polys[f]) {
                face_sum[v] += face_points[f];
                ++face_count[v];
            }
        }
        EditableMesh next = current;
        next.positions.clear();
        for (std::size_t v = 0; v < nv; ++v) {
            const glm::vec3& p = current.positions[v];
            if (face_count[v] == 0) {
                next.positions.push_back(p);
            } else if (boundary_vertex[v]) {
                next.positions.push_back(p * 0.75f + boundary_sum[v] * 0.125f);
            } else {
                const float n = static_cast<float>(face_count[v]);
                const glm::vec3 f = face_sum[v] / n;
                const glm::vec3 r = edge_mid_sum[v] / static_cast<float>(std::max(edge_count[v], 1u));
                next.positions.push_back((f + 2.0f * r + (n - 3.0f) * p) / n);
            }
        }
        const auto edge_base = static_cast<std::uint32_t>(next.positions.size());
        next.positions.insert(next.positions.end(), edge_points.begin(), edge_points.end());
        const auto face_base = static_cast<std::uint32_t>(next.positions.size());
        next.positions.insert(next.positions.end(), face_points.begin(), face_points.end());
        std::vector<std::vector<std::uint32_t>> quads;
        for (std::size_t f = 0; f < polys.size(); ++f) {
            const auto& poly = polys[f];
            const std::size_t n = poly.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t prev = poly[(i + n - 1) % n], v = poly[i], nxt = poly[(i + 1) % n];
                quads.push_back({v, edge_base + edge_ids[undirected_key(v, nxt)],
                                 face_base + static_cast<std::uint32_t>(f),
                                 edge_base + edge_ids[undirected_key(prev, v)]});
            }
        }
        set_faces(next, quads);
        current = std::move(next);
    }
    return current;
}

EditableMesh mirror(const EditableMesh& mesh, int axis, float weld) {
    EditableMesh out = mesh;
    if (!is_valid(mesh) || axis < 0 || axis > 2) {
        return out;
    }
    const auto n = static_cast<std::uint32_t>(mesh.positions.size());
    std::vector<std::uint32_t> mirrored(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        glm::vec3 p = mesh.positions[i];
        if (std::abs(p[axis]) <= weld) {
            out.positions[i][axis] = 0.0f; // on the plane: shared by both halves
            mirrored[i] = i;
            continue;
        }
        p[axis] = -p[axis];
        mirrored[i] = static_cast<std::uint32_t>(out.positions.size());
        out.positions.push_back(p);
    }
    auto polys = polygons(mesh);
    const std::size_t count = polys.size();
    for (std::size_t f = 0; f < count; ++f) {
        std::vector<std::uint32_t> copy;
        for (auto it = polys[f].rbegin(); it != polys[f].rend(); ++it) {
            copy.push_back(mirrored[*it]);
        }
        polys.push_back(std::move(copy));
    }
    set_faces(out, polys);
    return out;
}

EditableMesh merge_vertices(const EditableMesh& mesh, float distance) {
    EditableMesh out = mesh;
    if (!is_valid(mesh) || mesh.positions.empty()) {
        return out;
    }
    const float cell = std::max(distance, 1e-9f);
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
    const auto key = [&](const glm::ivec3& c) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x) & 0x1FFFFF) << 42) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y) & 0x1FFFFF) << 21) |
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.z) & 0x1FFFFF);
    };
    std::vector<std::uint32_t> remap(mesh.positions.size());
    for (std::uint32_t i = 0; i < mesh.positions.size(); ++i) {
        const glm::vec3& p = mesh.positions[i];
        const glm::ivec3 c = glm::ivec3(glm::floor(p / cell));
        remap[i] = i;
        bool found = false;
        for (int dz = -1; dz <= 1 && !found; ++dz) {
            for (int dy = -1; dy <= 1 && !found; ++dy) {
                for (int dx = -1; dx <= 1 && !found; ++dx) {
                    const auto it = grid.find(key(c + glm::ivec3(dx, dy, dz)));
                    if (it == grid.end()) {
                        continue;
                    }
                    for (const std::uint32_t j : it->second) {
                        if (glm::distance(mesh.positions[j], p) <= distance) {
                            remap[i] = j;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!found) {
            grid[key(c)].push_back(i);
        }
    }
    std::vector<std::vector<std::uint32_t>> faces;
    for (auto poly : polygons(mesh)) {
        std::vector<std::uint32_t> merged;
        for (const std::uint32_t c : poly) {
            const std::uint32_t r = remap[c];
            if (merged.empty() || merged.back() != r) {
                merged.push_back(r);
            }
        }
        while (merged.size() > 1 && merged.front() == merged.back()) {
            merged.pop_back();
        }
        if (merged.size() >= 3) {
            faces.push_back(std::move(merged));
        }
    }
    set_faces(out, faces);
    compact(out);
    return out;
}

EditableMesh translate(const EditableMesh& mesh, const glm::vec3& offset) {
    EditableMesh out = mesh;
    for (glm::vec3& p : out.positions) {
        p += offset;
    }
    return out;
}

core::Bounds bounds(const EditableMesh& mesh) {
    core::Bounds b;
    for (const glm::vec3& p : mesh.positions) {
        b.add(p);
    }
    return b;
}

EditableMesh from_triangles(const assets::MeshData& data) {
    EditableMesh mesh;
    for (const assets::MeshVertex& v : data.vertices) {
        mesh.positions.push_back(v.position);
    }
    std::vector<std::vector<std::uint32_t>> tris;
    for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3) {
        tris.push_back({data.indices[i], data.indices[i + 1], data.indices[i + 2]});
    }
    set_faces(mesh, tris);
    const core::Bounds extent = bounds(mesh);
    mesh = merge_vertices(mesh, extent.empty() ? 1e-6f : 1e-5f * glm::length(extent.extent()));
    // Pair coplanar triangles that share an edge into quads (convex ones only).
    auto polys = polygons(mesh);
    std::unordered_map<std::uint64_t, std::uint32_t> edge_owner;
    for (std::size_t f = 0; f < polys.size(); ++f) {
        for (std::size_t i = 0; i < 3 && polys[f].size() == 3; ++i) {
            edge_owner[edge_key(polys[f][i], polys[f][(i + 1) % 3])] = static_cast<std::uint32_t>(f);
        }
    }
    std::vector<bool> used(polys.size(), false);
    std::vector<std::vector<std::uint32_t>> out;
    for (std::size_t f = 0; f < polys.size(); ++f) {
        if (used[f] || polys[f].size() != 3) {
            continue;
        }
        used[f] = true;
        const auto& t = polys[f];
        const glm::vec3 nf = glm::normalize(glm::cross(mesh.positions[t[1]] - mesh.positions[t[0]],
                                                       mesh.positions[t[2]] - mesh.positions[t[0]]));
        bool merged = false;
        for (std::size_t i = 0; i < 3 && !merged; ++i) {
            const std::uint32_t a = t[i], b = t[(i + 1) % 3], c = t[(i + 2) % 3];
            const auto it = edge_owner.find(edge_key(b, a));
            if (it == edge_owner.end() || used[it->second]) {
                continue;
            }
            const auto& u = polys[it->second];
            std::uint32_t d = u[0];
            for (const std::uint32_t k : u) {
                if (k != a && k != b) {
                    d = k;
                }
            }
            const glm::vec3 ng = glm::normalize(glm::cross(mesh.positions[u[1]] - mesh.positions[u[0]],
                                                           mesh.positions[u[2]] - mesh.positions[u[0]]));
            if (!(glm::dot(nf, ng) > 0.9999f)) {
                continue;
            }
            const std::vector<std::uint32_t> quad{a, d, b, c};
            // Convex in the plane?
            bool convex = true;
            for (std::size_t k = 0; k < 4 && convex; ++k) {
                const glm::vec3& p0 = mesh.positions[quad[k]];
                const glm::vec3& p1 = mesh.positions[quad[(k + 1) % 4]];
                const glm::vec3& p2 = mesh.positions[quad[(k + 2) % 4]];
                convex = glm::dot(glm::cross(p1 - p0, p2 - p1), nf) > 0.0f;
            }
            if (convex) {
                used[it->second] = true;
                out.push_back(quad);
                merged = true;
            }
        }
        if (!merged) {
            out.push_back(t);
        }
    }
    set_faces(mesh, out);
    mesh.smoothing_angle = 30.0f;
    return mesh;
}

void register_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        scene::register_types();
        entt::meta_factory<Shape>{}
            .type("ModelingShape")
            .data<Shape::None>("None")
            .data<Shape::Plane>("Plane")
            .data<Shape::Cube>("Cube")
            .data<Shape::Sphere>("Sphere")
            .data<Shape::Cylinder>("Cylinder")
            .data<Shape::Cone>("Cone")
            .data<Shape::Torus>("Torus")
            .data<Shape::Capsule>("Capsule")
            .data<Shape::Stairs>("Stairs");
        entt::meta_factory<std::vector<glm::vec3>>{}.type("vec3[]");
        entt::meta_factory<std::vector<std::uint32_t>>{}.type("uint32[]");
        entt::meta_factory<EditableMesh>{}
            .type("EditableMesh")
            .data<&EditableMesh::positions>("positions")
            .custom<FieldMeta>(FieldMeta{}.with_hidden())
            .data<&EditableMesh::face_sizes>("face_sizes")
            .custom<FieldMeta>(FieldMeta{}.with_hidden())
            .data<&EditableMesh::corners>("corners")
            .custom<FieldMeta>(FieldMeta{}.with_hidden())
            .data<&EditableMesh::smoothing_angle>("smoothing_angle")
            .custom<FieldMeta>(FieldMeta{}.with_unit("deg").with_range(0.0, 180.0, 1.0).with_tooltip(
                "Edges sharper than this look hard; 0 is flat shading, 180 smooth everywhere"))
            .data<&EditableMesh::uv_scale>("uv_scale")
            .custom<FieldMeta>(FieldMeta{}.with_unit("m").with_range(0.01, 100.0, 0.01).with_tooltip(
                "Metres per texture repeat"))
            .data<&EditableMesh::shape>("shape")
            .custom<FieldMeta>(FieldMeta{}.with_hidden())
            .data<&EditableMesh::shape_size>("shape_size")
            .custom<FieldMeta>(FieldMeta{}.with_hidden())
            .data<&EditableMesh::shape_segments_a>("shape_segments_a")
            .custom<FieldMeta>(FieldMeta{}.with_hidden())
            .data<&EditableMesh::shape_segments_b>("shape_segments_b")
            .custom<FieldMeta>(FieldMeta{}.with_hidden());
        scene::register_component<EditableMesh>("EditableMesh", false);
        return true;
    }();
    static_cast<void>(registered);
}

std::string mesh_path(scene::World& world, entt::entity entity) {
    return "modeling://" + world.guid(entity).to_string();
}

void sync_meshes(scene::World& world, assets::MeshLibrary& meshes) {
    auto& registry = world.registry();
    const auto view = registry.view<EditableMesh>();
    if (view.empty()) {
        return;
    }
    F3D_ZONE;
    auto& cache = registry.ctx().contains<SyncCache>() ? registry.ctx().get<SyncCache>()
                                                       : registry.ctx().emplace<SyncCache>();
    for (const entt::entity e : view) {
        const EditableMesh& mesh = view.get<EditableMesh>(e);
        const core::Guid guid = world.guid(e);
        const std::string path = "modeling://" + guid.to_string();
        const std::uint64_t hash = hash_mesh(mesh);
        auto [it, inserted] = cache.hashes.try_emplace({&meshes, guid}, ~hash);
        if (it->second != hash || !meshes.contains(path)) {
            const auto* name = registry.try_get<scene::Name>(e);
            meshes.add(path, triangulate(mesh, name ? name->value : std::string_view{}));
            it->second = hash;
        }
        auto* renderer = registry.try_get<scene::MeshRenderer>(e);
        if (!renderer) {
            renderer = &registry.emplace<scene::MeshRenderer>(e);
        }
        if (!renderer->mesh.guid.is_null() || renderer->mesh.path != path) {
            renderer->mesh = core::AssetRef{core::Guid{}, path};
        }
    }
}

} // namespace f3d::modeling
