// SPDX-License-Identifier: Apache-2.0
#pragma once

// Watches the source files of a project's assets, and the files they depend
// on (glTF buffers, OBJ materials, textures), for edits made outside the
// editor (phases.md P3.5). A changed asset is then rescanned (its hash goes
// stale), re-imported and hot-swapped on the GPU.
//
// sync() runs on the main thread whenever the database changes; poll() stats
// every watched file, so a large project polls on a worker. The two may run
// concurrently: sync() only hands over the new watch list, poll() applies it.

#include "f3d/assets/asset_database.hpp"
#include "f3d/core/file_watcher.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/vfs.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace f3d::assets {

class SourceWatcher {
public:
    /// Watches the sources and dependencies of every record that lives on disk
    /// (paths `fs` resolves to native files). Cheap if the database is unchanged.
    void sync(const core::vfs::FileSystem& fs, const AssetDatabase& database);
    /// Stops watching everything (project closed).
    void clear();

    /// Assets whose source or a dependency changed (or appeared / disappeared)
    /// since the previous poll, each once. Files first seen in this poll aren't
    /// reported: their state is the baseline.
    [[nodiscard]] std::vector<core::Guid> poll();

    [[nodiscard]] std::size_t watched() const;

private:
    using Watches = std::unordered_map<std::filesystem::path::string_type, std::vector<core::Guid>>;

    mutable std::mutex mutex_;
    std::optional<Watches> pending_; ///< from sync(), applied by the next poll()
    std::uint64_t synced_version_ = ~std::uint64_t{0};
    std::size_t watched_ = 0;

    // Owned by poll().
    std::mutex poll_mutex_;
    Watches watches_;
    core::FileWatcher watcher_;
};

} // namespace f3d::assets
