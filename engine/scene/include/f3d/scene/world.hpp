// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/selection.hpp"

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f3d::scene {

/// The scene: an entt::registry plus the structure every entity shares.
///
/// Every entity has Name, core::Guid, Transform and Hierarchy. Entities form an
/// ordered forest: each parent keeps its children in order, and the roots form
/// an ordered chain owned by the World. That order is part of the scene (the
/// outliner shows it, saves keep it, undo restores it).
///
/// Entities are referred to across undo/redo and saves by Guid; entt::entity
/// handles are only valid until the entity is destroyed.
class World {
public:
    static constexpr std::size_t kAppend = std::numeric_limits<std::size_t>::max();

    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

    /// The selection set (changed through the selection.* operations).
    [[nodiscard]] Selection& selection() noexcept { return selection_; }
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }

    /// The layer table (changed through the layer.* operations).
    [[nodiscard]] LayerTable& layers() noexcept { return layers_; }
    [[nodiscard]] const LayerTable& layers() const noexcept { return layers_; }
    /// Saved camera views (changed through the bookmark.* operations).
    [[nodiscard]] BookmarkTable& bookmarks() noexcept { return bookmarks_; }
    [[nodiscard]] const BookmarkTable& bookmarks() const noexcept { return bookmarks_; }
    /// A camera view requested by bookmark.recall, for the editor to apply and clear.
    /// Transient: not part of the saved scene.
    [[nodiscard]] std::optional<CameraBookmark>& view_request() noexcept { return view_request_; }

    // --- Lifetime ---------------------------------------------------------------------------

    /// Creates an entity under `parent` (null: a root) at sibling position `index`
    /// (kAppend: last). A null `guid` generates a new one. Returns null if `guid`
    /// is already used or `parent` is invalid.
    entt::entity create(std::string_view name, entt::entity parent = entt::null, core::Guid guid = {},
                        std::size_t index = kAppend);

    /// Destroys `e` and its whole subtree.
    void destroy(entt::entity e);

    /// Destroys every entity and clears the selection, layers and bookmarks.
    void clear();

    [[nodiscard]] bool valid(entt::entity e) const noexcept;
    /// Number of entities.
    [[nodiscard]] std::size_t size() const noexcept { return guid_map_.size(); }

    // --- Identity ---------------------------------------------------------------------------

    /// The entity with this Guid, or null.
    [[nodiscard]] entt::entity find(const core::Guid& guid) const;
    [[nodiscard]] const core::Guid& guid(entt::entity e) const;
    [[nodiscard]] const std::string& name(entt::entity e) const;
    void set_name(entt::entity e, std::string name);

    // --- Hierarchy --------------------------------------------------------------------------

    [[nodiscard]] entt::entity parent(entt::entity e) const;
    /// Children of `e` in order; the roots if `e` is null.
    [[nodiscard]] std::vector<entt::entity> children(entt::entity e) const;
    [[nodiscard]] std::vector<entt::entity> roots() const { return children(entt::null); }
    [[nodiscard]] std::size_t child_count(entt::entity e) const;
    /// Position of `e` among its siblings.
    [[nodiscard]] std::size_t sibling_index(entt::entity e) const;
    /// True if `ancestor` is `e` itself or one of its ancestors.
    [[nodiscard]] bool is_ancestor_or_self(entt::entity ancestor, entt::entity e) const;
    /// Depth of `e` (roots are 0).
    [[nodiscard]] std::size_t depth(entt::entity e) const;

    /// Moves `e` under `parent` (null: root) at sibling position `index` (kAppend: last;
    /// the index is taken after `e` is removed from its old place). With `keep_world`,
    /// the local transform is recomputed so the world transform doesn't change.
    /// Returns false (and changes nothing) if `parent` is `e` or one of its descendants.
    bool set_parent(entt::entity e, entt::entity parent, std::size_t index = kAppend, bool keep_world = true);

    /// Every entity in depth-first pre-order (parents before children, siblings in order).
    [[nodiscard]] std::vector<entt::entity> depth_first() const;
    /// The subtree of `e` in depth-first pre-order, starting with `e`.
    [[nodiscard]] std::vector<entt::entity> subtree(entt::entity e) const;

    // --- Transforms -------------------------------------------------------------------------

    [[nodiscard]] const Transform& transform(entt::entity e) const;
    void set_local(entt::entity e, const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);
    /// Sets the local transform so the entity ends up at `world` (decomposed into TRS).
    void set_world_matrix(entt::entity e, const glm::mat4& world);
    /// Marks the local transform as changed (after writing Transform's TRS fields directly).
    void mark_dirty(entt::entity e);
    /// Recomputes the cached world matrix of every dirty entity and its descendants.
    void update_transforms();
    /// World matrix of `e`, brought up to date first if needed.
    [[nodiscard]] const glm::mat4& world_matrix(entt::entity e);
    [[nodiscard]] bool transforms_dirty() const noexcept { return transforms_dirty_; }

private:
    void link(entt::entity e, entt::entity parent, std::size_t index);
    void unlink(entt::entity e);
    void on_guid_destroyed(entt::registry& registry, entt::entity e);
    Hierarchy& hierarchy(entt::entity e) { return registry_.get<Hierarchy>(e); }
    [[nodiscard]] const Hierarchy& hierarchy(entt::entity e) const { return registry_.get<Hierarchy>(e); }

    entt::registry registry_;
    std::unordered_map<core::Guid, entt::entity> guid_map_;
    Selection selection_;
    LayerTable layers_;
    BookmarkTable bookmarks_;
    std::optional<CameraBookmark> view_request_;
    entt::entity first_root_ = entt::null;
    entt::entity last_root_ = entt::null;
    std::uint32_t root_count_ = 0;
    bool transforms_dirty_ = false;
};

/// Splits an affine matrix into translation, rotation and (signed) scale.
/// Shear is discarded.
void decompose(const glm::mat4& m, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale);

} // namespace f3d::scene
