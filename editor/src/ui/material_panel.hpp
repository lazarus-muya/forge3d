// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/assets/mesh_library.hpp"
#include "f3d/render/raster/material_preview.hpp"
#include "panel.hpp"

#include <functional>
#include <string>

namespace f3d::scene {
class World;
}

namespace f3d::assets {
class AssetDatabase;
}

namespace f3d::editor {

class SceneEditor;

namespace panel_ids {
inline constexpr const char* kMaterialEditor = "material_editor";
inline constexpr const char* kMaterialLibrary = "material_library";
} // namespace panel_ids

/// What the material editor asks the App to do.
struct MaterialEditorActions {
    /// Copies the material of `slot` of `entity`'s MeshRenderer into a project
    /// .f3dmat and assigns it to that slot (one undo step).
    std::function<void(const core::Guid& entity, std::size_t slot)> make_editable_copy;
    /// Assigns the project material `material` to slot `slot` of `entity` (one undo step).
    std::function<void(const core::Guid& entity, std::size_t slot, const core::Guid& material)> assign_slot;
};

/// What the material library asks the App to do. Materials live in project://materials/.
struct MaterialLibraryActions {
    /// Creates a project material: a copy of `copy_from`, or the default material if it is
    /// empty. Returns its VFS path (empty on failure; e.g. an unsaved scene).
    std::function<std::string(const core::AssetRef& copy_from)> create;
    /// Assigns the material to every slot of every selected mesh (one undo step).
    std::function<void(const core::Guid& material)> assign_to_selection;
    /// Deletes the material file; slots using it go back to the default (one undo step).
    std::function<void(const core::Guid& material)> remove;
};

/// Material editor (phases.md P4.2): the active entity's material slots, a
/// preview sphere, and the selected slot's parameters. Project materials
/// (.f3dmat) are edited through material.set_property; a model's own materials
/// are read-only until copied into the project.
class MaterialEditorPanel final : public Panel {
public:
    MaterialEditorPanel(scene::World* world, assets::MeshLibrary* meshes, SceneEditor* editor,
                        render::raster::MaterialPreview* preview, MaterialEditorActions actions);

protected:
    void draw_contents() override;

private:
    scene::World* world_ = nullptr;
    assets::MeshLibrary* meshes_ = nullptr;
    SceneEditor* editor_ = nullptr;
    render::raster::MaterialPreview* preview_ = nullptr;
    MaterialEditorActions actions_;
    core::Guid entity_;
    std::size_t slot_ = 0;
};

/// Material library: the project's materials (.f3dmat) with a preview and their
/// parameters. New, duplicate, rename, delete; assign to the selection (double-click,
/// context menu) or drag one onto a mesh in the viewport.
class MaterialLibraryPanel final : public Panel {
public:
    MaterialLibraryPanel(const assets::AssetDatabase* assets, assets::MeshLibrary* meshes, SceneEditor* editor,
                         render::raster::MaterialPreview* preview, MaterialLibraryActions actions);

protected:
    void draw_contents() override;

private:
    void create(const core::AssetRef& copy_from);

    const assets::AssetDatabase* assets_ = nullptr;
    assets::MeshLibrary* meshes_ = nullptr;
    SceneEditor* editor_ = nullptr;
    render::raster::MaterialPreview* preview_ = nullptr;
    MaterialLibraryActions actions_;
    core::Guid selected_;
    std::string select_path_; ///< a material just created: selected once the asset scan lists it
    std::string filter_;
    core::Guid renaming_;
    std::string rename_buffer_;
};

/// True if `ref` names a project material file the editor can write.
[[nodiscard]] bool is_editable_material(const core::AssetRef& ref);

} // namespace f3d::editor
