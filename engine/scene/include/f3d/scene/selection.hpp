// SPDX-License-Identifier: Apache-2.0
#pragma once

// The scene's selection set (phases.md P2.4). Changed only through the
// selection.* operations so it is undoable; the editor reads it for the
// outliner, inspector and gizmos.

#include "f3d/core/guid.hpp"

#include <cstddef>
#include <vector>

namespace f3d::scene {

class World;

/// An ordered set of selected entities. `entities()[0]` is the primary selection
/// (the "active element" for pivot modes); `active()` is the last-clicked entity.
class Selection {
public:
    [[nodiscard]] const std::vector<core::Guid>& entities() const noexcept { return entities_; }
    [[nodiscard]] core::Guid active() const noexcept { return active_; }
    [[nodiscard]] bool empty() const noexcept { return entities_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }
    [[nodiscard]] bool contains(const core::Guid& guid) const;

    void set(std::vector<core::Guid> entities, core::Guid active = {});
    void add(const core::Guid& guid);
    void remove(const core::Guid& guid);
    void clear();
    void set_active(const core::Guid& guid);

    /// Drops entities that no longer exist in `world`.
    void prune(const World& world);

private:
    std::vector<core::Guid> entities_;
    core::Guid active_;
};

} // namespace f3d::scene
