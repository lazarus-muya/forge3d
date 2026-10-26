// SPDX-License-Identifier: Apache-2.0
#pragma once

// The asset pipeline (phases.md P3.2): import + cook one asset into the project's
// cache, and load cooked assets back.
//
//   project://.forge3d/cache/<guid>/model.json      models: node hierarchy, mesh list, materials
//   project://.forge3d/cache/<guid>/mesh_<n>.f3dmesh
//   project://.forge3d/cache/<guid>/image_<n>.f3dtex  models: the images their materials use
//   project://.forge3d/cache/<guid>/texture.f3dtex  textures
//
// A successful import records the source hash (and any defaulted settings) in
// the asset's .meta, so the asset is no longer stale.

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/cook.hpp"
#include "f3d/assets/mesh.hpp"
#include "f3d/assets/texture.hpp"
#include "f3d/core/jobs.hpp"
#include "f3d/core/vfs.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace f3d::assets {

/// "project://.forge3d/cache/<guid>" (no trailing slash).
[[nodiscard]] std::string cooked_dir(const core::Guid& guid, std::string_view mount = "project");

struct ImportOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<std::string> outputs; ///< VFS paths written
    std::string source_hash;          ///< recorded in the .meta
};

/// Progress in [0, 1], called from the importing thread.
using ImportProgress = std::function<void(float)>;

/// Imports and cooks one asset. The source must be readable through `fs` (models
/// also need a native path: their importers read sibling files). Safe to run on a
/// worker; distinct assets can be imported in parallel.
ImportOutcome import_asset(core::vfs::FileSystem& fs, const AssetRecord& asset,
                           const core::jobs::CancellationToken* cancel = nullptr, const ImportProgress& progress = {});

/// A cooked model: the node hierarchy, its cooked meshes and its materials (textures
/// as "#image/<n>" sub-asset paths, see rebase_textures()).
struct CookedModel {
    std::string name;
    std::vector<ModelNode> nodes;
    std::vector<CookedMesh> meshes;
    std::vector<Material> materials;
};

/// Loads a cooked model / texture from the cache. Empty with `error` set if it
/// isn't cooked (or the files are damaged).
[[nodiscard]] std::optional<CookedModel> load_cooked_model(const core::vfs::FileSystem& fs, const core::Guid& guid,
                                                           std::string* error = nullptr);
[[nodiscard]] std::optional<CookedTexture> load_cooked_texture(const core::vfs::FileSystem& fs, const core::Guid& guid,
                                                               std::string* error = nullptr);
/// Image `index` of a cooked model.
[[nodiscard]] std::optional<CookedTexture> load_cooked_image(const core::vfs::FileSystem& fs, const core::Guid& guid,
                                                             std::size_t index, std::string* error = nullptr);
/// The cooked texture a reference points at: a texture asset ({guid, path}) or an image
/// of a cooked model ({guid, "<model>#image/<n>"}).
[[nodiscard]] std::optional<CookedTexture> load_cooked_texture(const core::vfs::FileSystem& fs,
                                                               const core::AssetRef& ref, std::string* error = nullptr);
/// Decodes the source of an image of a model file (embedded bytes, or a file next to
/// `model_file`).
[[nodiscard]] std::optional<SourceImage>
decode_model_image(const ModelImage& image, const std::filesystem::path& model_file, std::string* error = nullptr);

} // namespace f3d::assets
