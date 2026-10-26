// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/mesh_library.hpp"
#include "f3d/modeling/editable_mesh.hpp"
#include "f3d/modeling/modeling_ops.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <set>

using namespace f3d;
using namespace f3d::modeling;

namespace {

/// Edges used once (open boundary) or more than twice (non-manifold), by position pair.
std::size_t open_edges(const EditableMesh& mesh) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> count;
    std::uint32_t offset = 0;
    for (const std::uint32_t size : mesh.face_sizes) {
        for (std::uint32_t i = 0; i < size; ++i) {
            std::uint32_t a = mesh.corners[offset + i], b = mesh.corners[offset + (i + 1) % size];
            if (a > b) {
                std::swap(a, b);
            }
            ++count[{a, b}];
        }
        offset += size;
    }
    std::size_t open = 0;
    for (const auto& [edge, n] : count) {
        open += n != 2 ? 1 : 0;
    }
    return open;
}

/// Signed volume of a closed mesh (positive when its faces point outwards).
float volume(const EditableMesh& mesh) {
    const assets::MeshData tris = triangulate(mesh);
    float v = 0.0f;
    for (std::size_t i = 0; i + 2 < tris.indices.size(); i += 3) {
        const glm::vec3& a = tris.vertices[tris.indices[i]].position;
        const glm::vec3& b = tris.vertices[tris.indices[i + 1]].position;
        const glm::vec3& c = tris.vertices[tris.indices[i + 2]].position;
        v += glm::dot(a, glm::cross(b, c)) / 6.0f;
    }
    return v;
}

/// The face of a closed mesh whose normal points most along `direction`.
std::uint32_t face_towards(const EditableMesh& mesh, const glm::vec3& direction) {
    std::uint32_t best = 0;
    float best_dot = -2.0f;
    for (std::uint32_t f = 0; f < mesh.face_count(); ++f) {
        const float d = glm::dot(face_normal(mesh, f), direction);
        if (d > best_dot) {
            best_dot = d;
            best = f;
        }
    }
    return best;
}

} // namespace

TEST_CASE("modeling: every shape is a valid, closed, outward-facing solid resting on y = 0") {
    for (const Shape shape : {Shape::Cube, Shape::Sphere, Shape::Cylinder, Shape::Cone, Shape::Torus, Shape::Capsule,
                              Shape::Stairs}) {
        CAPTURE(to_string(shape));
        const EditableMesh mesh = make_shape(default_shape(shape));
        REQUIRE(is_valid(mesh));
        CHECK(mesh.shape == shape);
        CHECK(open_edges(mesh) == 0);
        CHECK(volume(mesh) > 0.0f);
        CHECK(bounds(mesh).min.y == doctest::Approx(0.0f).epsilon(1e-4));
    }
    const EditableMesh plane = make_shape(default_shape(Shape::Plane));
    REQUIRE(is_valid(plane));
    CHECK(face_normal(plane, 0).y == doctest::Approx(1.0f));
}

TEST_CASE("modeling: shape dimensions and volumes") {
    const EditableMesh cube = make_shape({Shape::Cube, glm::vec3(2.0f, 1.0f, 3.0f), 1, 1});
    CHECK(cube.positions.size() == 8);
    CHECK(cube.face_count() == 6);
    CHECK(volume(cube) == doctest::Approx(6.0f));
    const EditableMesh subdivided_cube = make_shape({Shape::Cube, glm::vec3(1.0f), 3, 1});
    CHECK(subdivided_cube.face_count() == 54);
    CHECK(open_edges(subdivided_cube) == 0);
    // A finely divided sphere approaches 4/3 pi r^3.
    const EditableMesh sphere = make_shape({Shape::Sphere, glm::vec3(2.0f), 64, 32});
    CHECK(volume(sphere) == doctest::Approx(4.0f / 3.0f * 3.14159265f).epsilon(0.01));
    const EditableMesh stairs = make_shape({Shape::Stairs, glm::vec3(1.0f, 1.0f, 1.0f), 4, 1});
    // Steps of 1/4: a staircase of area (1 + 2 + 3 + 4) / 16 times the width.
    CHECK(volume(stairs) == doctest::Approx(10.0f / 16.0f));
}

TEST_CASE("modeling: triangulation keeps the surface, smooths by angle and projects UVs") {
    const EditableMesh cube = make_shape(default_shape(Shape::Cube));
    const assets::MeshData flat = triangulate(cube);
    CHECK(flat.vertices.size() == 24); // one vertex per corner
    CHECK(flat.indices.size() == 36);
    CHECK(flat.uvs.size() == flat.vertices.size());
    // Flat: each vertex has its face's normal.
    for (const assets::MeshVertex& v : flat.vertices) {
        CHECK(glm::length(v.normal) == doctest::Approx(1.0f));
        CHECK(std::max({std::abs(v.normal.x), std::abs(v.normal.y), std::abs(v.normal.z)}) ==
              doctest::Approx(1.0f));
    }
    EditableMesh smooth = cube;
    smooth.smoothing_angle = 180.0f;
    const assets::MeshData rounded = triangulate(smooth);
    for (const assets::MeshVertex& v : rounded.vertices) {
        CHECK(std::abs(v.normal.x) == doctest::Approx(1.0f / std::sqrt(3.0f)).epsilon(1e-3));
    }
}

TEST_CASE("modeling: extrude, inset, delete, flip") {
    const EditableMesh cube = make_shape(default_shape(Shape::Cube));
    const std::uint32_t top = face_towards(cube, glm::vec3(0.0f, 1.0f, 0.0f));

    SUBCASE("extruding the top face raises it and adds four walls") {
        const EditableMesh out = extrude_faces(cube, {top}, 0.5f);
        REQUIRE(is_valid(out));
        CHECK(out.face_count() == 10);
        CHECK(open_edges(out) == 0);
        CHECK(face_centre(out, top).y == doctest::Approx(1.5f)); // the extruded face keeps its index
        CHECK(volume(out) == doctest::Approx(1.5f));
    }
    SUBCASE("extruding two neighbouring faces moves them as one region") {
        const std::uint32_t side = face_towards(cube, glm::vec3(1.0f, 0.0f, 0.0f));
        const EditableMesh out = extrude_faces(cube, {top, side}, 0.25f);
        REQUIRE(is_valid(out));
        CHECK(open_edges(out) == 0);
        CHECK(out.face_count() == 6 + 6); // the region's boundary has six edges
    }
    SUBCASE("inset keeps the outline and adds a ring") {
        const EditableMesh out = inset_faces(cube, {top}, 0.5f);
        REQUIRE(is_valid(out));
        CHECK(out.face_count() == 10);
        CHECK(open_edges(out) == 0);
        CHECK(volume(out) == doctest::Approx(1.0f));
        const core::Bounds inner = [&] {
            core::Bounds b;
            const auto offsets = face_offsets(out);
            for (std::uint32_t i = 0; i < out.face_sizes[top]; ++i) {
                b.add(out.positions[out.corners[offsets[top] + i]]);
            }
            return b;
        }();
        CHECK(inner.extent().x == doctest::Approx(0.5f));
    }
    SUBCASE("deleting a face opens the mesh; flipping reverses the volume") {
        const EditableMesh open = delete_faces(cube, {top});
        CHECK(open.face_count() == 5);
        CHECK(open_edges(open) == 4);
        CHECK(volume(flip_faces(cube, {})) == doctest::Approx(-1.0f));
        CHECK(face_normal(flip_faces(cube, {top}), top).y == doctest::Approx(-1.0f));
    }
}

TEST_CASE("modeling: loop cut") {
    const EditableMesh cube = make_shape(default_shape(Shape::Cube));
    const std::uint32_t top = face_towards(cube, glm::vec3(0.0f, 1.0f, 0.0f));
    const auto offsets = face_offsets(cube);
    // An edge of the top face; the ring runs around the cube across it.
    const std::uint32_t a = cube.corners[offsets[top]];
    const std::uint32_t b = cube.corners[offsets[top] + 1];

    SUBCASE("the ring of a cube edge goes round four faces and closes") {
        const auto ring = edge_ring(cube, a, b);
        CHECK(ring.size() == 4);
        CHECK(ring.front() == std::pair{a, b});
        CHECK(edge_ring(cube, a, a).empty());
    }
    SUBCASE("one cut splits the four faces of the ring where the factor says") {
        const EditableMesh out = loop_cut(cube, a, b, 1, 0.25f);
        REQUIRE(is_valid(out));
        CHECK(out.face_count() == 10);
        CHECK(out.positions.size() == 12);
        CHECK(open_edges(out) == 0);
        CHECK(volume(out) == doctest::Approx(1.0f));
        CHECK(glm::distance(out.positions[8], glm::mix(cube.positions[a], cube.positions[b], 0.25f)) < 1e-5f);
    }
    SUBCASE("several cuts are spaced evenly") {
        const EditableMesh out = loop_cut(cube, a, b, 3, 0.9f);
        REQUIRE(is_valid(out));
        CHECK(out.face_count() == 2 + 4 * 4);
        CHECK(open_edges(out) == 0);
        CHECK(glm::distance(out.positions[8], glm::mix(cube.positions[a], cube.positions[b], 0.25f)) < 1e-5f);
    }
    SUBCASE("a ring ending at a non-quad puts the new positions in its outline") {
        // Two quads side by side and a triangle on the far edge of the second.
        EditableMesh strip;
        strip.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 0, 1}, {1, 0, 1}, {2, 0, 1}, {3, 0, 0.5f}};
        strip.face_sizes = {4, 4, 3};
        strip.corners = {0, 3, 4, 1, 1, 4, 5, 2, 2, 5, 6};
        const auto ring = edge_ring(strip, 0, 3);
        CHECK(ring.size() == 3); // (0,3), (1,4), (2,5)
        const EditableMesh out = loop_cut(strip, 0, 3, 1, 0.5f);
        REQUIRE(is_valid(out));
        CHECK(out.face_count() == 5);
        CHECK(out.face_sizes.back() == 4); // the triangle gained the cut's end
    }
    SUBCASE("unknown edges change nothing") {
        CHECK(loop_cut(cube, a, cube.corners[offsets[top] + 2] + 100, 1) == cube);
    }
}

TEST_CASE("modeling: subdivision, mirror, merge, origin") {
    const EditableMesh cube = make_shape(default_shape(Shape::Cube));
    const EditableMesh once = subdivide(cube, 1);
    CHECK(once.face_count() == 24);
    CHECK(once.positions.size() == 26);
    CHECK(open_edges(once) == 0);
    CHECK(volume(once) < 1.0f); // Catmull-Clark shrinks towards a rounded shape
    CHECK(volume(once) > 0.35f); // about 0.42 after one level

    // Half a cube (x >= 0, open at x = 0), mirrored: a closed box again.
    const EditableMesh shifted = translate(cube, glm::vec3(0.5f, 0.0f, 0.0f));
    const EditableMesh half = delete_faces(shifted, {face_towards(shifted, glm::vec3(-1.0f, 0.0f, 0.0f))});
    const EditableMesh mirrored = mirror(half, 0, 1e-4f);
    REQUIRE(is_valid(mirrored));
    CHECK(mirrored.positions.size() == 12); // the four on the plane are shared
    CHECK(volume(mirrored) == doctest::Approx(2.0f));

    // Two coincident cubes merge into one set of positions; faces stay.
    EditableMesh doubled = cube;
    const auto n = static_cast<std::uint32_t>(cube.positions.size());
    doubled.positions.insert(doubled.positions.end(), cube.positions.begin(), cube.positions.end());
    for (const std::uint32_t c : cube.corners) {
        doubled.corners.push_back(c + n);
    }
    doubled.face_sizes.insert(doubled.face_sizes.end(), cube.face_sizes.begin(), cube.face_sizes.end());
    CHECK(merge_vertices(doubled, 1e-4f).positions.size() == 8);
}

TEST_CASE("modeling: raycast and conversion from triangles") {
    const EditableMesh cube = make_shape(default_shape(Shape::Cube));
    const auto hit = raycast(cube, glm::vec3(0.1f, 5.0f, 0.1f), glm::vec3(0.0f, -1.0f, 0.0f));
    REQUIRE(hit);
    CHECK(hit->distance == doctest::Approx(4.0f));
    CHECK(face_normal(cube, hit->face).y == doctest::Approx(1.0f));
    CHECK_FALSE(raycast(cube, glm::vec3(3.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)));

    // Triangles back to polygons: the cube's quads come back.
    const EditableMesh back = from_triangles(triangulate(cube));
    CHECK(back.positions.size() == 8);
    CHECK(back.face_count() == 6);
    CHECK(open_edges(back) == 0);
}

TEST_CASE("modeling: operations edit, undo and redo; sync draws the mesh") {
    register_types();
    scene::World world;
    ops::ChangeBus changes;
    ops::OpRegistry registry;
    ops::register_builtin_ops(registry);
    register_modeling_ops(registry);
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    const auto run = [&](const char* id, const nlohmann::json& params) {
        auto op = registry.create(id, params);
        return op && stack.execute(std::move(op));
    };
    const core::Guid guid = core::Guid::generate();
    REQUIRE(run("entity.create", {{"name", "Box"}, {"guid", guid.to_string()}}));
    REQUIRE(run("modeling.set_shape", {{"entity", guid.to_string()}, {"shape", "cube"}, {"size", {2, 2, 2}}}));
    const entt::entity e = world.find(guid);
    REQUIRE(world.registry().all_of<EditableMesh>(e));
    CHECK(world.registry().get<EditableMesh>(e).shape == Shape::Cube);
    CHECK_FALSE(run("modeling.set_shape", {{"entity", guid.to_string()}, {"shape", "teapot"}}));
    CHECK_FALSE(run("modeling.extrude", {{"entity", guid.to_string()}, {"faces", nlohmann::json::array()}}));

    const EditableMesh before = world.registry().get<EditableMesh>(e);
    REQUIRE(run("modeling.extrude", {{"entity", guid.to_string()}, {"faces", {0}}, {"distance", 1.0}}));
    CHECK(world.registry().get<EditableMesh>(e).face_count() == 10);
    CHECK(world.registry().get<EditableMesh>(e).shape == Shape::None); // edited: a plain mesh now
    REQUIRE(stack.undo());
    CHECK(world.registry().get<EditableMesh>(e) == before);
    REQUIRE(stack.redo());
    CHECK(world.registry().get<EditableMesh>(e).face_count() == 10);
    CHECK_FALSE(run("modeling.loop_cut", {{"entity", guid.to_string()}, {"a", 0}, {"b", 0}}));

    // set_origin keeps the geometry in place in the world.
    REQUIRE(run("modeling.set_origin", {{"entity", guid.to_string()}, {"mode", "centre"}}));
    CHECK(bounds(world.registry().get<EditableMesh>(e)).center().y == doctest::Approx(0.0f));
    CHECK(world.transform(e).translation.y > 0.0f);
    REQUIRE(stack.undo());
    CHECK(world.transform(e).translation.y == doctest::Approx(0.0f));

    // Drawing: sync triangulates into the mesh library and points a MeshRenderer at it.
    assets::MeshLibrary meshes;
    sync_meshes(world, meshes);
    const auto* renderer = world.registry().try_get<scene::MeshRenderer>(e);
    REQUIRE(renderer);
    CHECK(renderer->mesh.path == mesh_path(world, e));
    const auto drawn = meshes.get(renderer->mesh);
    REQUIRE(drawn);
    const std::size_t indices = drawn->indices.size();
    REQUIRE(stack.undo()); // the extrusion
    sync_meshes(world, meshes);
    CHECK(meshes.get(renderer->mesh)->indices.size() < indices);
}
