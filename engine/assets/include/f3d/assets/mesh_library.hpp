// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/assets/material.hpp"
#include "f3d/assets/mesh.hpp"
#include "f3d/core/asset_ref.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace f3d::assets {

/// Resolves mesh asset references ("<file>#mesh/<n>") to CPU mesh data, loading
/// each source file once. Main thread only. Replaced by the asset database and
/// cooked meshes in M3.
class MeshLibrary {
public:
    /// The mesh `ref` points at, or nullptr if it can't be loaded (logged once).
    [[nodiscard]] std::shared_ptr<const MeshData> get(const core::AssetRef& ref);

    /// The glTF scene of `file` (cached), or nullptr with `error` set.
    [[nodiscard]] std::shared_ptr<const GltfScene> load_file(const std::filesystem::path& file,
                                                             std::string* error = nullptr);

    /// Registers an in-memory mesh under `path` (tests, generated meshes).
    void add(const std::string& path, MeshData mesh);
    /// True if a mesh is registered (or loaded) under `path`.
    [[nodiscard]] bool contains(const std::string& path) const { return meshes_.contains(path); }

    /// Resolves references with a GUID (cooked project assets, P3.3): called on a
    /// cache miss with the reference; the result (or nullptr) is cached by GUID + sub-path.
    using Resolver = std::function<std::shared_ptr<const MeshData>(const core::AssetRef&)>;
    void set_resolver(Resolver resolver) { resolver_ = std::move(resolver); }
    /// The material `ref` points at, or nullptr if it can't be loaded (logged once):
    /// "<model file>#material/<n>" and ".f3dmat" files by path; references with a GUID
    /// through the material resolver. Texture slots come back rebased onto the model
    /// ("<model file>#image/<n>", with the model's GUID).
    [[nodiscard]] std::shared_ptr<const Material> material(const core::AssetRef& ref);
    /// Resolves references with a GUID or a VFS path ("project://..."): cooked model
    /// materials and project .f3dmat files.
    using MaterialResolver = std::function<std::shared_ptr<const Material>(const core::AssetRef&)>;
    void set_material_resolver(MaterialResolver resolver) { material_resolver_ = std::move(resolver); }
    /// Registers an in-memory material under `path` (previews, tests).
    void add_material(const std::string& path, Material material);
    /// Forgets every cached material (a material file changed).
    void invalidate_materials();
    /// The material submesh `material_index` of `mesh` uses by default: the same model's
    /// "#material/<n>" (with the mesh's GUID). Empty for SubMesh::kNoMaterial.
    [[nodiscard]] static core::AssetRef default_material(const core::AssetRef& mesh, std::uint32_t material_index);
    /// The material of slot `slot` of a MeshRenderer: its override if set, else the
    /// submesh's default material. Empty reference: the built-in default material.
    [[nodiscard]] static core::AssetRef slot_material(const scene::MeshRenderer& renderer, std::size_t slot,
                                                      const SubMesh& submesh);

    /// Decodes an image: of a model loaded by path ("<file>#image/<n>"; `kind` receives
    /// the texture kind its material slot wants), else through the image resolver (GUID
    /// textures, VFS paths), else an image file by path. Empty (logged once) on failure.
    [[nodiscard]] std::optional<SourceImage> image(const core::AssetRef& ref, TextureKind* kind = nullptr);
    /// Resolves images that aren't model images: texture assets by GUID, "project://" paths.
    /// Returns nullopt to fall back to reading `ref.path` as a file.
    using ImageResolver = std::function<std::optional<SourceImage>(const core::AssetRef&)>;
    void set_image_resolver(ImageResolver resolver) { image_resolver_ = std::move(resolver); }

    /// Forgets the cached meshes and materials of an asset (after a re-import or edit).
    void invalidate(const core::Guid& guid);

    /// Increments whenever the set of meshes changes (renderers drop stale GPU copies).
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    void clear();

private:
    std::unordered_map<std::string, std::shared_ptr<const MeshData>> meshes_;
    std::unordered_map<std::string, std::shared_ptr<const GltfScene>> files_;
    std::unordered_map<std::string, std::shared_ptr<const Material>> materials_;
    std::unordered_set<std::string> failed_;
    std::uint64_t generation_ = 0;
    Resolver resolver_;
    MaterialResolver material_resolver_;
    ImageResolver image_resolver_;
};

/// The operations that create an instance of a model hierarchy (`nodes`, as in
/// ModelScene) under `parent` (null: a root): a root entity named `name` at
/// `root_translation` holding the nodes, MeshRenderers referencing
/// `mesh_ref(mesh index)`. A forge3d.ops list for ops::run_script; `root_guid`
/// receives the root's GUID.
[[nodiscard]] nlohmann::json model_instance_ops(const std::string& name, const std::vector<ModelNode>& nodes,
                                                const std::function<core::AssetRef(std::size_t)>& mesh_ref,
                                                const core::Guid& parent, const glm::vec3& root_translation,
                                                core::Guid& root_guid);

/// The operations that import a glTF scene under `parent` (null: a root), as a
/// forge3d.ops list for ops::run_script: entity.create for a root named after the
/// file and every node, entity.set_property for non-identity transforms, and
/// entity.add_component for MeshRenderers (gltf_mesh_path(file, mesh)). GUIDs are
/// generated up front; `root_guid` receives the root's. Run it in a transaction
/// to make the import one undo step.
[[nodiscard]] nlohmann::json gltf_import_ops(const GltfScene& gltf, const std::filesystem::path& file,
                                             const core::Guid& parent, core::Guid& root_guid);

/// Creates entities for a glTF scene under `parent` directly, bypassing the
/// operation history: one root entity named after the file, holding the file's
/// node hierarchy. Nodes with a mesh get a MeshRenderer pointing at
/// gltf_mesh_path(file, mesh). Returns the root. Prefer gltf_import_ops for
/// anything the user can undo.
entt::entity spawn_gltf(scene::World& world, const GltfScene& gltf, const std::filesystem::path& file,
                        entt::entity parent = entt::null);

/// World-space bounds of every MeshRenderer in the subtree of `root` (the whole
/// scene if `root` is null). Empty if there are no meshes.
[[nodiscard]] core::Bounds world_bounds(scene::World& world, MeshLibrary& meshes, entt::entity root = entt::null);

} // namespace f3d::assets
