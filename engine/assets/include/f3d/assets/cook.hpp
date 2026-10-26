// SPDX-License-Identifier: Apache-2.0
#pragma once

// Mesh cooking (phases.md P3.2): an imported MeshData -> optional xatlas UVs ->
// vertex cache / fetch optimisation -> LODs (meshoptimizer simplification) ->
// the .f3dmesh file format.

#include "f3d/assets/mesh.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace f3d::assets {

/// Import settings for models, stored in the model's .meta "settings".
struct MeshCookSettings {
    std::uint32_t max_lods = 4; ///< LOD0 + up to 3 simplified levels
    float lod_ratio = 0.5f;     ///< target triangle ratio between consecutive levels
    float lod_error = 0.05f;    ///< allowed simplification error, relative to the mesh size
    bool generate_uvs = false;  ///< xatlas unwrap (replaces existing UVs; lightmaps, baking)

    [[nodiscard]] static MeshCookSettings from_json(const nlohmann::json& json);
    [[nodiscard]] nlohmann::json to_json() const;
};

struct MeshLod {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    float error = 0.0f; ///< simplification error of this level (relative)
    /// The level's material ranges (absolute positions in CookedMesh::indices, inside
    /// this level's range). Empty: the whole level, no material.
    std::vector<SubMesh> submeshes;
};

/// A cooked mesh: one vertex buffer shared by all LODs, their index ranges back to back.
struct CookedMesh {
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<glm::vec2> uvs; ///< empty or one per vertex
    std::vector<std::uint32_t> indices;
    std::vector<MeshLod> lods; ///< LOD0 first
    core::Bounds bounds;

    [[nodiscard]] std::uint32_t triangle_count(std::size_t lod) const noexcept {
        return lod < lods.size() ? lods[lod].index_count / 3 : 0;
    }
    /// One LOD as a renderable MeshData (indices of that level only).
    [[nodiscard]] MeshData to_mesh_data(std::size_t lod = 0) const;
    /// The submeshes of a level relative to its first index (one, without a material, if it has none).
    [[nodiscard]] std::vector<SubMesh> lod_submeshes(std::size_t lod) const;
};

/// Generates a UV atlas for `mesh` with xatlas (vertices are split at seams).
/// Returns false (mesh unchanged) if xatlas fails.
bool generate_uvs(MeshData& mesh, std::string* error = nullptr);

/// Cooks one mesh.
[[nodiscard]] CookedMesh cook_mesh(MeshData mesh, const MeshCookSettings& settings);

/// The .f3dmesh file format (docs/architecture/assets.md).
[[nodiscard]] std::vector<std::uint8_t> write_f3dmesh(const CookedMesh& mesh);
[[nodiscard]] std::optional<CookedMesh> read_f3dmesh(std::span<const std::uint8_t> data, std::string* error = nullptr);

} // namespace f3d::assets
