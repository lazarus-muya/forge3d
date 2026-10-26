// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3d::core {

/// Polling file watcher: remembers each file's last-write time and size and
/// reports files whose stamp changed (including creation and deletion) since
/// the previous poll. Cheap enough to poll a few hundred files every frame or so.
class FileWatcher {
public:
    /// Starts watching `path` (a file). Watching an already watched file is a no-op.
    void watch(const std::filesystem::path& path);
    void unwatch(const std::filesystem::path& path);
    void clear() noexcept { entries_.clear(); }
    [[nodiscard]] bool watching(const std::filesystem::path& path) const;

    /// Files that changed since they were added or last reported.
    [[nodiscard]] std::vector<std::filesystem::path> poll();

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Stamp {
        std::filesystem::file_time_type time;
        std::uintmax_t size = 0;
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };
    struct Entry {
        std::filesystem::path path;
        std::optional<Stamp> stamp; ///< nullopt: file missing
    };
    static std::optional<Stamp> stamp_of(const std::filesystem::path& path);

    std::unordered_map<std::filesystem::path::string_type, Entry> entries_; ///< by native path
};

} // namespace f3d::core
