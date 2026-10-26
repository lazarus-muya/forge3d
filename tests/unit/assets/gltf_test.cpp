// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/assets/mesh.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using f3d::assets::load_gltf;

namespace {
fs::path gltf_dir() {
    return fs::path(F3D_TEST_DATA_DIR) / "gltf";
}
} // namespace

TEST_CASE("gltf: Box loads as 12 triangles with unit normals") {
    const auto r = load_gltf(gltf_dir() / "Box.glb");
    INFO(r.error);
    REQUIRE(r.mesh.has_value());
    const auto& m = *r.mesh;
    CHECK(m.name == "Box");
    CHECK(m.primitive_count == 1);
    CHECK(m.vertices.size() == 24);
    CHECK(m.indices.size() == 36);
    // A unit cube; the root node's rotation keeps it inside [-0.5, 0.5].
    for (int i = 0; i < 3; ++i) {
        CHECK(m.bounds.min[i] == doctest::Approx(-0.5f));
        CHECK(m.bounds.max[i] == doctest::Approx(0.5f));
    }
    for (const auto& v : m.vertices) {
        CHECK(glm::length(v.normal) == doctest::Approx(1.0f).epsilon(1e-4));
    }
    for (const auto idx : m.indices) {
        CHECK(idx < m.vertices.size());
    }
}

TEST_CASE("gltf: Fox loads (skinned model in bind pose)") {
    const auto r = load_gltf(gltf_dir() / "Fox.glb");
    INFO(r.error);
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->vertices.size() > 1000);
    CHECK(r.mesh->indices.size() % 3 == 0);
    CHECK_FALSE(r.mesh->bounds.empty());
}

TEST_CASE("gltf: missing normals and indices are generated") {
    // One triangle in the XY plane, counter-clockwise seen from +Z; no NORMAL, no indices.
    // 3 positions (36 bytes) as base64.
    const auto dir = fs::temp_directory_path() / "forge3d-tests" / "gltf";
    fs::create_directories(dir);
    const auto path = dir / "triangle.gltf";
    std::ofstream(path) << R"({
  "asset": {"version": "2.0"},
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "translation": [0, 2, 0]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
  "bufferViews": [{"buffer": 0, "byteLength": 36}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 0]}]
})";
    const auto r = load_gltf(path);
    INFO(r.error);
    REQUIRE(r.mesh.has_value());
    const auto& m = *r.mesh;
    REQUIRE(m.vertices.size() == 3);
    REQUIRE(m.indices.size() == 3);
    for (const auto& v : m.vertices) {
        CHECK(glm::length(v.normal - glm::vec3(0, 0, 1)) < 1e-5f); // generated, facing +Z
        CHECK(v.position.y >= 2.0f);                               // node translation applied
    }
}

TEST_CASE("gltf: errors") {
    CHECK_FALSE(load_gltf(gltf_dir() / "missing.glb").mesh);
    const auto dir = fs::temp_directory_path() / "forge3d-tests" / "gltf";
    fs::create_directories(dir);
    std::ofstream(dir / "broken.gltf") << "{ not gltf";
    const auto r = load_gltf(dir / "broken.gltf");
    CHECK_FALSE(r.mesh);
    CHECK_FALSE(r.error.empty());
}
