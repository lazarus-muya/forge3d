// SPDX-License-Identifier: Apache-2.0
#include "f3d/scene/selection.hpp"

#include "f3d/scene/world.hpp"

#include <algorithm>

namespace f3d::scene {

bool Selection::contains(const core::Guid& guid) const {
    return std::find(entities_.begin(), entities_.end(), guid) != entities_.end();
}

void Selection::set(std::vector<core::Guid> entities, core::Guid active) {
    entities_ = std::move(entities);
    active_ = active.is_null() ? (entities_.empty() ? core::Guid{} : entities_.front()) : active;
}

void Selection::add(const core::Guid& guid) {
    if (guid.is_null() || contains(guid)) {
        return;
    }
    entities_.push_back(guid);
    if (active_.is_null()) {
        active_ = guid;
    }
}

void Selection::remove(const core::Guid& guid) {
    entities_.erase(std::remove(entities_.begin(), entities_.end(), guid), entities_.end());
    if (active_ == guid) {
        active_ = entities_.empty() ? core::Guid{} : entities_.front();
    }
}

void Selection::clear() {
    entities_.clear();
    active_ = {};
}

void Selection::set_active(const core::Guid& guid) {
    active_ = guid;
}

void Selection::prune(const World& world) {
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                   [&](const core::Guid& guid) { return world.find(guid) == entt::null; }),
                    entities_.end());
    if (!active_.is_null() && world.find(active_) == entt::null) {
        active_ = entities_.empty() ? core::Guid{} : entities_.front();
    }
}

} // namespace f3d::scene
