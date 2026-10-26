// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/file_watcher.hpp"

namespace f3d::core {

std::optional<FileWatcher::Stamp> FileWatcher::stamp_of(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return Stamp{time, size};
}

void FileWatcher::watch(const std::filesystem::path& path) {
    if (!entries_.contains(path.native())) {
        entries_.emplace(path.native(), Entry{path, stamp_of(path)});
    }
}

void FileWatcher::unwatch(const std::filesystem::path& path) {
    entries_.erase(path.native());
}

bool FileWatcher::watching(const std::filesystem::path& path) const {
    return entries_.contains(path.native());
}

std::vector<std::filesystem::path> FileWatcher::poll() {
    std::vector<std::filesystem::path> changed;
    for (auto& [key, e] : entries_) {
        auto now = stamp_of(e.path);
        if (now != e.stamp) {
            e.stamp = now;
            changed.push_back(e.path);
        }
    }
    return changed;
}

} // namespace f3d::core
