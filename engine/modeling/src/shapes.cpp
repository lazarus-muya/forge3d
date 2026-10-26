// SPDX-License-Identifier: Apache-2.0
// Shape generators (editable_mesh.hpp). Every shape rests on y = 0, centred on x and z.

#include "f3d/modeling/editable_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace f3d::modeling {
namespace {

constexpr float kPi = 3.14159265358979f;

constexpr std::array<std::pair<Shape, std::string_view>, 9> kShapes = {{
    {Shape::None, "none"},
    {Shape::Plane, "plane"},
    {Shape::Cube, "cube"},
    {Shape::Sphere, "sphere"},
    {Shape::Cylinder, "cylinder"},
    {Shape::Cone, "cone"},
    {Shape::Torus, "torus"},
    {Shape::Capsule, "capsule"},
    {Shape::Stairs, "stairs"},
}};

class Builder {
public:
    std::uint32_t vertex(const glm::vec3& p) {
        mesh.positions.push_back(p);
        return static_cast<std::uint32_t>(mesh.positions.size() - 1);
    }
    /// Adds a face, wound so its normal points along `outward`.
    void face(std::vector<std::uint32_t> corners, const glm::vec3& outward) {
        glm::vec3 n(0.0f);
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const glm::vec3& a = mesh.positions[corners[i]];
            const glm::vec3& b = mesh.positions[corners[(i + 1) % corners.size()]];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, outward) < 0.0f) {
            std::reverse(corners.begin(), corners.end());
        }
        mesh.face_sizes.push_back(static_cast<std::uint32_t>(corners.size()));
        mesh.corners.insert(mesh.corners.end(), corners.begin(), corners.end());
    }
    /// A grid of quads from `origin` spanning `u` and `v`, facing `outward`.
    void grid(const glm::vec3& origin, const glm::vec3& u, const glm::vec3& v, std::uint32_t nu, std::uint32_t nv,
              const glm::vec3& outward) {
        const std::uint32_t first = static_cast<std::uint32_t>(mesh.positions.size());
        for (std::uint32_t j = 0; j <= nv; ++j) {
            for (std::uint32_t i = 0; i <= nu; ++i) {
                vertex(origin + u * (static_cast<float>(i) / static_cast<float>(nu)) +
                       v * (static_cast<float>(j) / static_cast<float>(nv)));
            }
        }
        for (std::uint32_t j = 0; j < nv; ++j) {
            for (std::uint32_t i = 0; i < nu; ++i) {
                const std::uint32_t a = first + j * (nu + 1) + i;
                face({a, a + 1, a + nu + 2, a + nu + 1}, outward);
            }
        }
    }
    /// A ring of `sides` positions at height y and radius r (a single position if r == 0).
    std::vector<std::uint32_t> ring(float y, float r, std::uint32_t sides) {
        if (r <= 0.0f) {
            return std::vector<std::uint32_t>(sides, vertex(glm::vec3(0.0f, y, 0.0f)));
        }
        std::vector<std::uint32_t> out;
        for (std::uint32_t s = 0; s < sides; ++s) {
            const float a = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(sides);
            out.push_back(vertex(glm::vec3(std::sin(a) * r, y, std::cos(a) * r)));
        }
        return out;
    }
    /// Faces between two rings (quads, or triangles where a ring is one position).
    void band(const std::vector<std::uint32_t>& lower, const std::vector<std::uint32_t>& upper) {
        const std::size_t n = lower.size();
        for (std::size_t s = 0; s < n; ++s) {
            const std::size_t t = (s + 1) % n;
            std::vector<std::uint32_t> corners;
            for (const std::uint32_t c : {lower[s], lower[t], upper[t], upper[s]}) {
                if (corners.empty() || corners.back() != c) {
                    corners.push_back(c);
                }
            }
            if (corners.size() > 1 && corners.front() == corners.back()) {
                corners.pop_back();
            }
            if (corners.size() < 3) {
                continue;
            }
            glm::vec3 centre(0.0f);
            for (const std::uint32_t c : corners) {
                centre += mesh.positions[c];
            }
            centre /= static_cast<float>(corners.size());
            face(corners, glm::vec3(centre.x, 0.0f, centre.z));
        }
    }

    EditableMesh mesh;
};

std::uint32_t at_least(std::uint32_t value, std::uint32_t minimum) {
    return std::max(value, minimum);
}

/// A sphere-like solid of revolution through rings (y, radius), pole to pole, with
/// outward hints from the axis point at `axis_y(y)`.
template <class AxisY>
void revolve(Builder& b, const std::vector<std::pair<float, float>>& rings, std::uint32_t sides, AxisY axis_y) {
    std::vector<std::vector<std::uint32_t>> ids;
    for (const auto& [y, r] : rings) {
        ids.push_back(b.ring(y, r, sides));
    }
    for (std::size_t k = 0; k + 1 < ids.size(); ++k) {
        const std::size_t first_face = b.mesh.face_sizes.size();
        b.band(ids[k], ids[k + 1]);
        // band() hints radially; near the poles, hint away from the axis point instead.
        std::uint32_t offset = 0;
        for (std::size_t f = 0; f < first_face; ++f) {
            offset += b.mesh.face_sizes[f];
        }
        for (std::size_t f = first_face; f < b.mesh.face_sizes.size(); ++f) {
            const std::uint32_t size = b.mesh.face_sizes[f];
            glm::vec3 centre(0.0f);
            for (std::uint32_t c = 0; c < size; ++c) {
                centre += b.mesh.positions[b.mesh.corners[offset + c]];
            }
            centre /= static_cast<float>(size);
            const glm::vec3 outward = centre - glm::vec3(0.0f, axis_y(centre.y), 0.0f);
            const glm::vec3 n = face_normal(b.mesh, static_cast<std::uint32_t>(f));
            if (glm::dot(n, outward) < 0.0f) {
                std::reverse(b.mesh.corners.begin() + offset, b.mesh.corners.begin() + offset + size);
            }
            offset += size;
        }
    }
}

} // namespace

std::string_view to_string(Shape shape) noexcept {
    for (const auto& [value, name] : kShapes) {
        if (value == shape) {
            return name;
        }
    }
    return "none";
}

std::optional<Shape> parse_shape(std::string_view text) noexcept {
    for (const auto& [value, name] : kShapes) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

ShapeParams default_shape(Shape shape) noexcept {
    switch (shape) {
    case Shape::Plane:
        return {shape, glm::vec3(2.0f, 0.0f, 2.0f), 1, 1};
    case Shape::Sphere:
        return {shape, glm::vec3(1.0f), 32, 16};
    case Shape::Cylinder:
    case Shape::Cone:
        return {shape, glm::vec3(1.0f), 32, 1};
    case Shape::Torus:
        return {shape, glm::vec3(1.0f, 0.3f, 1.0f), 32, 16};
    case Shape::Capsule:
        return {shape, glm::vec3(0.6f, 1.8f, 0.6f), 24, 6};
    case Shape::Stairs:
        return {shape, glm::vec3(1.2f, 1.6f, 2.4f), 8, 1};
    case Shape::None:
    case Shape::Cube:
        break;
    }
    return {Shape::Cube, glm::vec3(1.0f), 1, 1};
}

EditableMesh make_shape(const ShapeParams& params) {
    Builder b;
    const glm::vec3 size = glm::max(params.size, glm::vec3(1e-4f));
    const std::uint32_t a = params.segments_a;
    const std::uint32_t c = params.segments_b;
    float smoothing = 30.0f;
    switch (params.shape) {
    case Shape::None:
    case Shape::Plane: {
        const std::uint32_t nx = at_least(a, 1), nz = at_least(c, 1);
        b.grid(glm::vec3(-size.x * 0.5f, 0.0f, -size.z * 0.5f), glm::vec3(0.0f, 0.0f, size.z),
               glm::vec3(size.x, 0.0f, 0.0f), nz, nx, glm::vec3(0.0f, 1.0f, 0.0f));
        break;
    }
    case Shape::Cube: {
        const std::uint32_t n = at_least(a, 1);
        const glm::vec3 h = size * 0.5f;
        const glm::vec3 lo(-h.x, 0.0f, -h.z);
        const glm::vec3 hi(h.x, size.y, h.z);
        const glm::vec3 x(size.x, 0.0f, 0.0f), y(0.0f, size.y, 0.0f), z(0.0f, 0.0f, size.z);
        b.grid(lo, y, z, n, n, glm::vec3(-1.0f, 0.0f, 0.0f));
        b.grid(glm::vec3(hi.x, lo.y, lo.z), y, z, n, n, glm::vec3(1.0f, 0.0f, 0.0f));
        b.grid(lo, x, z, n, n, glm::vec3(0.0f, -1.0f, 0.0f));
        b.grid(glm::vec3(lo.x, hi.y, lo.z), x, z, n, n, glm::vec3(0.0f, 1.0f, 0.0f));
        b.grid(lo, x, y, n, n, glm::vec3(0.0f, 0.0f, -1.0f));
        b.grid(glm::vec3(lo.x, lo.y, hi.z), x, y, n, n, glm::vec3(0.0f, 0.0f, 1.0f));
        b.mesh = merge_vertices(b.mesh, 1e-5f * glm::length(size));
        smoothing = 0.0f;
        break;
    }
    case Shape::Sphere: {
        const std::uint32_t sides = at_least(a, 3), rings = at_least(c, 2);
        const glm::vec3 r = size * 0.5f;
        std::vector<std::pair<float, float>> profile;
        for (std::uint32_t k = 0; k <= rings; ++k) {
            const float theta = kPi * static_cast<float>(k) / static_cast<float>(rings);
            profile.emplace_back(r.y - std::cos(theta) * r.y, std::sin(theta) * r.x);
        }
        revolve(b, profile, sides, [&](float) { return r.y; });
        // Ellipsoid: scale z separately (rings are circular in x).
        for (glm::vec3& p : b.mesh.positions) {
            p.z *= r.z / r.x;
        }
        smoothing = 180.0f;
        break;
    }
    case Shape::Cylinder:
    case Shape::Cone: {
        const std::uint32_t sides = at_least(a, 3), stacks = at_least(c, 1);
        const float r = size.x * 0.5f;
        const bool cone = params.shape == Shape::Cone;
        std::vector<std::pair<float, float>> profile;
        for (std::uint32_t k = 0; k <= stacks; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(stacks);
            profile.emplace_back(t * size.y, cone ? r * (1.0f - t) : r);
        }
        std::vector<std::vector<std::uint32_t>> ids;
        for (const auto& [y, radius] : profile) {
            ids.push_back(b.ring(y, radius, sides));
        }
        for (std::size_t k = 0; k + 1 < ids.size(); ++k) {
            b.band(ids[k], ids[k + 1]);
        }
        b.face(ids.front(), glm::vec3(0.0f, -1.0f, 0.0f)); // bottom cap (an n-gon)
        if (!cone) {
            b.face(ids.back(), glm::vec3(0.0f, 1.0f, 0.0f));
        }
        for (glm::vec3& p : b.mesh.positions) {
            p.z *= size.z / size.x;
        }
        smoothing = 45.0f;
        break;
    }
    case Shape::Torus: {
        const std::uint32_t around = at_least(a, 3), tube = at_least(c, 3);
        const float minor = std::min(size.y, size.x * 0.5f) * 0.5f;
        const float major = std::max(size.x * 0.5f - minor, 1e-3f);
        std::vector<std::uint32_t> ids;
        for (std::uint32_t i = 0; i < around; ++i) {
            const float u = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(around);
            for (std::uint32_t j = 0; j < tube; ++j) {
                const float v = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(tube);
                const float radial = major + minor * std::cos(v);
                ids.push_back(b.vertex(glm::vec3(std::sin(u) * radial, minor + minor * std::sin(v),
                                                 std::cos(u) * radial)));
            }
        }
        for (std::uint32_t i = 0; i < around; ++i) {
            const std::uint32_t i1 = (i + 1) % around;
            for (std::uint32_t j = 0; j < tube; ++j) {
                const std::uint32_t j1 = (j + 1) % tube;
                const std::uint32_t q0 = ids[i * tube + j], q1 = ids[i1 * tube + j];
                const std::uint32_t q2 = ids[i1 * tube + j1], q3 = ids[i * tube + j1];
                const glm::vec3 centre =
                    (b.mesh.positions[q0] + b.mesh.positions[q1] + b.mesh.positions[q2] + b.mesh.positions[q3]) * 0.25f;
                const glm::vec3 flat(centre.x, 0.0f, centre.z);
                const glm::vec3 ring_point =
                    glm::length(flat) > 0.0f ? glm::normalize(flat) * major + glm::vec3(0.0f, minor, 0.0f)
                                             : glm::vec3(0.0f, minor, 0.0f);
                b.face({q0, q1, q2, q3}, centre - ring_point);
            }
        }
        for (glm::vec3& p : b.mesh.positions) {
            p.z *= size.z / size.x;
        }
        smoothing = 180.0f;
        break;
    }
    case Shape::Capsule: {
        const std::uint32_t sides = at_least(a, 3), cap_rings = at_least(c, 2);
        const float r = size.x * 0.5f;
        const float height = std::max(size.y, 2.0f * r);
        std::vector<std::pair<float, float>> profile;
        for (std::uint32_t k = 0; k <= cap_rings; ++k) { // bottom pole to the lower equator
            const float theta = 0.5f * kPi * static_cast<float>(k) / static_cast<float>(cap_rings);
            profile.emplace_back(r - std::cos(theta) * r, std::sin(theta) * r);
        }
        for (std::uint32_t k = 0; k <= cap_rings; ++k) { // the upper equator to the top pole
            const float theta = 0.5f * kPi * static_cast<float>(k) / static_cast<float>(cap_rings);
            profile.emplace_back(height - r + std::sin(theta) * r, std::cos(theta) * r);
        }
        revolve(b, profile, sides, [&](float y) { return std::clamp(y, r, height - r); });
        for (glm::vec3& p : b.mesh.positions) {
            p.z *= size.z / size.x;
        }
        smoothing = 180.0f;
        break;
    }
    case Shape::Stairs: {
        const std::uint32_t steps = at_least(a, 1);
        const float rise = size.y / static_cast<float>(steps);
        const float run = size.z / static_cast<float>(steps);
        // Side profile in (z, y), clockwise: up the front, over the steps, down the back.
        std::vector<glm::vec2> profile{{0.0f, 0.0f}};
        for (std::uint32_t s = 0; s < steps; ++s) {
            profile.emplace_back(run * static_cast<float>(s), rise * static_cast<float>(s + 1));
            profile.emplace_back(run * static_cast<float>(s + 1), rise * static_cast<float>(s + 1));
        }
        profile.emplace_back(size.z, 0.0f);
        const float hx = size.x * 0.5f;
        std::vector<std::uint32_t> left, right;
        for (const glm::vec2& p : profile) {
            left.push_back(b.vertex(glm::vec3(-hx, p.y, p.x - size.z * 0.5f)));
            right.push_back(b.vertex(glm::vec3(hx, p.y, p.x - size.z * 0.5f)));
        }
        b.face(left, glm::vec3(-1.0f, 0.0f, 0.0f));
        b.face(right, glm::vec3(1.0f, 0.0f, 0.0f));
        for (std::size_t i = 0; i < profile.size(); ++i) {
            const std::size_t j = (i + 1) % profile.size();
            const glm::vec2 e = profile[j] - profile[i];
            b.face({left[i], left[j], right[j], right[i]}, glm::vec3(0.0f, e.x, -e.y)); // CW: outward (-ey, ex)
        }
        smoothing = 0.0f;
        break;
    }
    }
    EditableMesh out = std::move(b.mesh);
    out.smoothing_angle = smoothing;
    out.shape = params.shape == Shape::None ? Shape::Plane : params.shape;
    out.shape_size = params.size;
    out.shape_segments_a = params.segments_a;
    out.shape_segments_b = params.segments_b;
    // Seams of revolved shapes repeat positions at the poles: weld them.
    if (params.shape != Shape::Cube && params.shape != Shape::Plane && params.shape != Shape::Stairs) {
        const Shape shape = out.shape;
        out = merge_vertices(out, 1e-6f * glm::length(size));
        out.shape = shape;
    }
    return out;
}

} // namespace f3d::modeling
