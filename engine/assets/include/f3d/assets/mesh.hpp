// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/assets/material.hpp"
#include "f3d/core/math.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f3d::assets {

/// Vertex of the minimal mesh path (P1.4). Matches shaders/mesh.vs.hlsl.
struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    std::uint32_t color; ///< RGBA8, sRGB-encoded vertex colour (white unless the source has colours)
};
static_assert(sizeof(MeshVertex) == 28);

/// A range of a mesh's indices drawn with one material (phases.md P4.2).
struct SubMesh {
    static constexpr std::uint32_t kNoMaterial = 0xFFFFFFFFu;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = kNoMaterial; ///< index into the model's materials

    friend bool operator==(const SubMesh&, const SubMesh&) = default;
};

/// Triangle-list mesh: one vertex/index buffer, plus optional streams the
/// renderer doesn't use yet (kept beside MeshVertex so its GPU layout is stable).
struct MeshData {
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<glm::vec2> uvs; ///< TEXCOORD_0, one per vertex, or empty
    /// Index ranges by material, back to back in index order; empty: one range,
    /// no material (see submeshes_of()).
    std::vector<SubMesh> submeshes;
    core::Bounds bounds;
    std::uint32_t primitive_count = 0;
};

/// The mesh's submeshes, or one covering every index when it has none.
[[nodiscard]] std::vector<SubMesh> submeshes_of(const MeshData& mesh);
/// A UV sphere (radius, centred at the origin) with normals and UVs; one submesh
/// without a material. Material previews and tests.
[[nodiscard]] MeshData make_uv_sphere(float radius = 1.0f, std::uint32_t segments = 48, std::uint32_t rings = 24);
/// Sorts triangles by `triangle_material` (one per triangle, stable) and returns
/// the resulting submeshes. Used by importers that assign materials per face.
std::vector<SubMesh> group_by_material(MeshData& mesh, const std::vector<std::uint32_t>& triangle_material);

struct MeshLoadResult {
    std::optional<MeshData> mesh;
    std::string error;
};

/// Loads a .gltf / .glb (fastgltf) flattened into one world-space mesh: every
/// triangle primitive of the default scene with its node's world transform
/// applied. Missing normals are computed (smooth, area-weighted); missing
/// indices are generated.
[[nodiscard]] MeshLoadResult load_gltf(const std::filesystem::path& path);

/// One node of an imported model, with its local transform.
struct ModelNode {
    std::string name;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    int parent = -1; ///< index into ModelScene::nodes, -1 for a root
    int mesh = -1;   ///< index into ModelScene::meshes, -1 for none
};

/// An image a model's materials use: a file next to the model, or bytes embedded in it.
struct ModelImage {
    std::string name;
    std::string uri;                       ///< relative to the model's directory ('/' separators), or empty
    std::vector<std::uint8_t> bytes;       ///< embedded data, or empty
    std::string extension;                 ///< ".png", ".jpg", ... (picks the decoder)
    TextureKind kind = TextureKind::Color; ///< from the first material slot that uses it
};

/// An imported model file (glTF, FBX, OBJ): the common intermediate of the mesh
/// importers (phases.md P3.2). A node hierarchy plus meshes in local (mesh) space.
/// Right-handed, Y up, metres, counter-clockwise front faces.
struct ModelScene {
    std::string name;             ///< file stem
    std::vector<ModelNode> nodes; ///< depth-first: parents before children
    std::vector<MeshData> meshes; ///< one per source mesh (all its triangle primitives)
    /// Materials (P4.2). Their textures are sub-asset paths "#image/<n>" into `images`,
    /// rebased onto the model's reference when used (rebase_sub_asset()).
    std::vector<Material> materials;
    std::vector<ModelImage> images;
    std::vector<std::string> warnings;
    /// Files the model refers to (textures, material libraries, buffers), relative
    /// to the model's directory, '/' separators.
    std::vector<std::string> dependencies;
};

struct ModelLoadResult {
    std::optional<ModelScene> scene;
    std::string error;
};

using GltfNode = ModelNode;
using GltfScene = ModelScene;
using GltfSceneLoadResult = ModelLoadResult;

/// Loads a .gltf / .glb keeping its node hierarchy (P2.1: models become entities).
[[nodiscard]] ModelLoadResult load_gltf_scene(const std::filesystem::path& path);
/// Loads an .fbx (ufbx), converted to Y up, metres.
[[nodiscard]] ModelLoadResult load_fbx_scene(const std::filesystem::path& path);
/// Loads an .obj (tinyobjloader): one node per shape.
[[nodiscard]] ModelLoadResult load_obj_scene(const std::filesystem::path& path);
/// Loads any supported model, by extension (.gltf, .glb, .fbx, .obj).
[[nodiscard]] ModelLoadResult load_model(const std::filesystem::path& path);
/// True if load_model() handles the extension of `path`.
[[nodiscard]] bool is_model_file(const std::filesystem::path& path);

/// Area-weighted smooth normals for the vertices [first_vertex, end) of `mesh`,
/// from the triangles [first_index, end).
void compute_normals(MeshData& mesh, std::size_t first_vertex = 0, std::size_t first_index = 0);

/// Asset path of material `index` / image `index` of a model file: "<path>#material/<index>",
/// "<path>#image/<index>".
[[nodiscard]] std::string model_material_path(const std::filesystem::path& file, std::size_t index);
[[nodiscard]] std::string model_image_path(const std::filesystem::path& file, std::size_t index);

/// Asset path of mesh `index` of a model file: "<path>#mesh/<index>" (forward slashes).
[[nodiscard]] std::string gltf_mesh_path(const std::filesystem::path& file, std::size_t index);
[[nodiscard]] inline std::string model_mesh_path(const std::filesystem::path& file, std::size_t index) {
    return gltf_mesh_path(file, index);
}

} // namespace f3d::assets
