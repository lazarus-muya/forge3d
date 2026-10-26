// SPDX-License-Identifier: Apache-2.0
#pragma once

// Layers and camera bookmarks (phases.md P2.5): scene data owned by the World,
// changed only through the layer.* / bookmark.* operations, saved with the scene.

#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"

#include <entt/entity/entity.hpp>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::scene {

class World;

/// One row of the layer table. Entities refer to it by `id` (scene::Layer).
struct LayerInfo {
    core::Guid id;
    std::string name;
    bool visible = true; ///< hidden layers are not drawn, picked or snapped to
    bool locked = false; ///< locked layers are drawn but can't be picked
};

/// The scene's ordered layer table. The default layer (null id) is implicit.
class LayerTable {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] const std::vector<LayerInfo>& layers() const noexcept { return layers_; }
    [[nodiscard]] std::size_t size() const noexcept { return layers_.size(); }
    [[nodiscard]] LayerInfo* find(const core::Guid& id);
    [[nodiscard]] const LayerInfo* find(const core::Guid& id) const;
    [[nodiscard]] std::size_t index_of(const core::Guid& id) const;

    /// Inserts at `index` (npos: last). Fails for a null or duplicate id.
    bool insert(LayerInfo layer, std::size_t index = npos);
    bool remove(const core::Guid& id);
    void clear() { layers_.clear(); }

private:
    std::vector<LayerInfo> layers_;
};

/// A saved editor-camera view (orbit camera parameters).
struct CameraBookmark {
    std::string name;
    glm::vec3 target{0.0f};
    float distance = 12.0f;
    float yaw = 0.0f;   ///< radians
    float pitch = 0.0f; ///< radians
    bool orthographic = false;
};

/// The scene's camera bookmarks, ordered, unique by name.
class BookmarkTable {
public:
    [[nodiscard]] const std::vector<CameraBookmark>& bookmarks() const noexcept { return bookmarks_; }
    [[nodiscard]] const CameraBookmark* find(std::string_view name) const;
    [[nodiscard]] std::size_t index_of(std::string_view name) const;
    /// Replaces the bookmark with the same name, or inserts at `index` (npos: last).
    void set(CameraBookmark bookmark, std::size_t index = LayerTable::npos);
    bool remove(std::string_view name);
    void clear() { bookmarks_.clear(); }

private:
    std::vector<CameraBookmark> bookmarks_;
};

/// The layer `e` is on (null: the default layer).
[[nodiscard]] core::Guid layer_of(const World& world, entt::entity e);
/// False if `e` or an ancestor is on a hidden layer.
[[nodiscard]] bool is_visible(const World& world, entt::entity e);
/// False if `e` or an ancestor is on a hidden or locked layer.
[[nodiscard]] bool is_pickable(const World& world, entt::entity e);

} // namespace f3d::scene
