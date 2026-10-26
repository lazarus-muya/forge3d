// SPDX-License-Identifier: Apache-2.0
#pragma once

// CPU raycasting against the scene's meshes (phases.md P2.4 surface and vertex
// snapping): a BVH over the meshes' world bounds, then the triangles of the
// meshes whose bounds the ray enters, nearest first.

#include "f3d/assets/mesh.hpp"
#include "f3d/core/math.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::assets {

class MeshLibrary;

/// A ray/mesh hit in mesh-local space.
struct MeshHit {
    bool hit = false;
    float t = 0.0f; ///< distance along the ray (in units of the ray direction)
    glm::vec3 normal{0.0f};
};

/// Intersects `origin + t * direction` with `mesh` (in its local space).
/// Only hits with t <= t_max count.
[[nodiscard]] MeshHit ray_mesh(const glm::vec3& origin, const glm::vec3& direction, const MeshData& mesh,
                               float t_max = 3.0e38f);

/// A hit against the scene, in world space.
struct SceneHit {
    bool hit = false;
    entt::entity entity = entt::null;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    float distance = 0.0f;
};

/// Entities a query should ignore (e.g. the ones being dragged).
using EntityFilter = std::function<bool(entt::entity)>;

/// A bounding-volume hierarchy over the world bounds of the scene's visible
/// meshes. Built from a snapshot of the transforms; rebuild after they change.
class SceneBvh {
public:
    /// Collects every visible MeshRenderer of `world` whose mesh loads (hidden
    /// layers are skipped). Entities for which `exclude` returns true are left out.
    void build(scene::World& world, MeshLibrary& meshes, const EntityFilter& exclude = {});

    /// Nearest hit along the ray (`direction` need not be normalised), skipping
    /// entities for which `skip` returns true.
    [[nodiscard]] SceneHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                                   const EntityFilter& skip = {}) const;

    /// The mesh vertex closest in angle to the ray, within `max_angle` radians of
    /// it (a screen-space radius around the cursor). `hit` is false if none.
    [[nodiscard]] SceneHit nearest_vertex(const glm::vec3& origin, const glm::vec3& direction, float max_angle,
                                          const EntityFilter& skip = {}) const;

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    /// Number of meshes whose triangles the last raycast tested (for tests).
    [[nodiscard]] std::size_t last_meshes_tested() const noexcept { return last_tested_; }

private:
    struct Item {
        entt::entity entity = entt::null;
        std::shared_ptr<const MeshData> mesh;
        glm::mat4 world{1.0f};
        glm::mat4 inverse{1.0f};
        core::Bounds bounds; ///< world space
    };
    struct Node {
        core::Bounds bounds;
        std::uint32_t first = 0; ///< leaf: first item; inner: left child (right = left + 1)
        std::uint32_t count = 0; ///< leaf: item count; 0 for an inner node
    };
    void build_node(std::uint32_t index, std::uint32_t first, std::uint32_t count);

    std::vector<Item> items_;
    std::vector<Node> nodes_;
    mutable std::size_t last_tested_ = 0;
};

/// Builds a BVH and raycasts it once (convenience for one-off queries).
[[nodiscard]] SceneHit raycast_scene(scene::World& world, MeshLibrary& meshes, const glm::vec3& origin,
                                     const glm::vec3& direction);

} // namespace f3d::assets
