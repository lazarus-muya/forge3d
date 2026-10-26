// SPDX-License-Identifier: Apache-2.0
#include "f3d/scene/layers.hpp"

#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>

namespace f3d::scene {

LayerInfo* LayerTable::find(const core::Guid& id) {
    const std::size_t i = index_of(id);
    return i == npos ? nullptr : &layers_[i];
}

const LayerInfo* LayerTable::find(const core::Guid& id) const {
    const std::size_t i = index_of(id);
    return i == npos ? nullptr : &layers_[i];
}

std::size_t LayerTable::index_of(const core::Guid& id) const {
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].id == id) {
            return i;
        }
    }
    return npos;
}

bool LayerTable::insert(LayerInfo layer, std::size_t index) {
    if (layer.id.is_null() || find(layer.id)) {
        return false;
    }
    index = std::min(index, layers_.size());
    layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    return true;
}

bool LayerTable::remove(const core::Guid& id) {
    const std::size_t i = index_of(id);
    if (i == npos) {
        return false;
    }
    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
}

const CameraBookmark* BookmarkTable::find(std::string_view name) const {
    const std::size_t i = index_of(name);
    return i == LayerTable::npos ? nullptr : &bookmarks_[i];
}

std::size_t BookmarkTable::index_of(std::string_view name) const {
    for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
        if (bookmarks_[i].name == name) {
            return i;
        }
    }
    return LayerTable::npos;
}

void BookmarkTable::set(CameraBookmark bookmark, std::size_t index) {
    if (const std::size_t i = index_of(bookmark.name); i != LayerTable::npos) {
        bookmarks_[i] = std::move(bookmark);
        return;
    }
    index = std::min(index, bookmarks_.size());
    bookmarks_.insert(bookmarks_.begin() + static_cast<std::ptrdiff_t>(index), std::move(bookmark));
}

bool BookmarkTable::remove(std::string_view name) {
    const std::size_t i = index_of(name);
    if (i == LayerTable::npos) {
        return false;
    }
    bookmarks_.erase(bookmarks_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
}

core::Guid layer_of(const World& world, entt::entity e) {
    const Layer* layer = world.registry().try_get<Layer>(e);
    return layer ? layer->layer : core::Guid{};
}

namespace {

/// Walks `e` and its ancestors; `reject` decides from a layer row.
template <class Reject>
bool layers_allow(const World& world, entt::entity e, Reject reject) {
    if (world.layers().size() == 0) {
        return true; // only the default layer
    }
    for (entt::entity n = e; n != entt::null; n = world.parent(n)) {
        if (const Layer* layer = world.registry().try_get<Layer>(n); layer && !layer->layer.is_null()) {
            if (const LayerInfo* info = world.layers().find(layer->layer); info && reject(*info)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool is_visible(const World& world, entt::entity e) {
    return layers_allow(world, e, [](const LayerInfo& info) { return !info.visible; });
}

bool is_pickable(const World& world, entt::entity e) {
    return layers_allow(world, e, [](const LayerInfo& info) { return !info.visible || info.locked; });
}

} // namespace f3d::scene
