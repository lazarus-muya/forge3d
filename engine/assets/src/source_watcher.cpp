// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/source_watcher.hpp"

#include <algorithm>
#include <unordered_set>

namespace f3d::assets {

void SourceWatcher::sync(const core::vfs::FileSystem& fs, const AssetDatabase& database) {
    if (database.version() == synced_version_) {
        return;
    }
    synced_version_ = database.version();
    Watches watches;
    const auto add = [&](const std::string& path, const core::Guid& guid) {
        if (const auto native = fs.native_path(path)) {
            auto& guids = watches[native->lexically_normal().native()];
            if (std::find(guids.begin(), guids.end(), guid) == guids.end()) {
                guids.push_back(guid);
            }
        }
    };
    for (const AssetRecord& record : database.records()) {
        add(record.path, record.guid);
        for (const std::string& dependency : record.dependencies) {
            add(dependency, record.guid);
        }
    }
    const std::lock_guard lock(mutex_);
    watched_ = watches.size();
    pending_ = std::move(watches);
}

void SourceWatcher::clear() {
    const std::lock_guard lock(mutex_);
    synced_version_ = ~std::uint64_t{0};
    watched_ = 0;
    pending_ = Watches{};
}

std::size_t SourceWatcher::watched() const {
    const std::lock_guard lock(mutex_);
    return watched_;
}

std::vector<core::Guid> SourceWatcher::poll() {
    const std::lock_guard poll_lock(poll_mutex_);
    std::optional<Watches> next;
    {
        const std::lock_guard lock(mutex_);
        next.swap(pending_);
    }
    if (next) {
        for (const auto& [path, guids] : watches_) {
            if (!next->contains(path)) {
                watcher_.unwatch(path);
            }
        }
        for (const auto& [path, guids] : *next) {
            watcher_.watch(path); // a no-op for files already watched: their stamp stays
        }
        watches_ = std::move(*next);
    }
    std::vector<core::Guid> changed;
    std::unordered_set<core::Guid> seen;
    for (const std::filesystem::path& path : watcher_.poll()) {
        if (const auto it = watches_.find(path.native()); it != watches_.end()) {
            for (const core::Guid& guid : it->second) {
                if (seen.insert(guid).second) {
                    changed.push_back(guid);
                }
            }
        }
    }
    return changed;
}

} // namespace f3d::assets
