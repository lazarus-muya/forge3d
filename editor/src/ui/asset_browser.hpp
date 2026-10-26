// SPDX-License-Identifier: Apache-2.0
#pragma once

// The asset browser (phases.md P3.3): grid/list of the project's assets with
// thumbnails, search, type and tag filters. Assets are dragged into the
// viewport (payload kAssetPayload: the GUID as text) to place them.

#include "f3d/assets/asset_index.hpp"
#include "f3d/core/guid.hpp"
#include "panel.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace f3d::editor {

class ThumbnailCache;

namespace panel_ids {
inline constexpr const char* kAssetBrowser = "asset_browser";
} // namespace panel_ids

/// ImGui drag & drop payload type for assets.
inline constexpr const char* kAssetPayload = "F3D_ASSET";

/// What the browser asks the app to do.
struct AssetBrowserActions {
    std::function<void(const core::Guid&)> place;    ///< place a model at the 3D cursor
    std::function<void(const core::Guid&)> reimport; ///< import again (even if up to date)
    std::function<void(const core::Guid&, const std::vector<std::string>&)> set_tags;
    std::function<void()> rescan;
    std::function<void(const core::Guid&)> assign_material; ///< a material to every selected mesh
};

class AssetBrowserPanel final : public Panel {
public:
    AssetBrowserPanel(const assets::AssetDatabase* database, assets::AssetIndex* const* index,
                      ThumbnailCache* thumbnails, AssetBrowserActions actions);

protected:
    void draw_contents() override;

private:
    void refresh();
    void draw_toolbar();
    void draw_grid();
    void draw_list();
    void item_interactions(const assets::AssetSummary& item);
    void draw_tag_editor();

    const assets::AssetDatabase* database_ = nullptr;
    assets::AssetIndex* const* index_ = nullptr; ///< the app's current index (may change with the project)
    ThumbnailCache* thumbnails_ = nullptr;
    AssetBrowserActions actions_;

    std::array<char, 128> search_{};
    int type_filter_ = 0; ///< 0 all, 1 models, 2 textures
    std::set<std::string> tag_filter_;
    bool grid_ = true;
    float thumbnail_size_ = 96.0f;

    // Cached query results: refreshed when the query or the database changes.
    std::vector<assets::AssetSummary> results_;
    std::string last_query_;
    std::uint64_t last_version_ = ~0ull;
    double last_query_ms_ = 0.0;

    core::Guid selected_;
    core::Guid editing_tags_;
    std::array<char, 256> tag_buffer_{};
};

} // namespace f3d::editor
