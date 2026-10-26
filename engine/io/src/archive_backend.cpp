// SPDX-License-Identifier: Apache-2.0
#include "f3d/io/archive_backend.hpp"

#include <algorithm>
#include <set>

namespace f3d::io {

ArchiveBackend::ArchiveBackend(const std::filesystem::path& path) : reader_(path) {}

bool ArchiveBackend::exists(std::string_view path) const {
    std::scoped_lock lock(mutex_);
    if (path.empty() || reader_.find(path)) {
        return true;
    }
    // A directory: some entry lives under it.
    const std::string prefix = std::string(path) + "/";
    return std::any_of(reader_.entries().begin(), reader_.entries().end(),
                       [&](const ArchiveEntry& e) { return e.name.starts_with(prefix); });
}

std::optional<core::vfs::Bytes> ArchiveBackend::read(std::string_view path) const {
    std::scoped_lock lock(mutex_);
    if (!reader_.find(path)) {
        return std::nullopt;
    }
    return reader_.read(path);
}

std::vector<core::vfs::FileInfo> ArchiveBackend::list(std::string_view dir, bool recursive) const {
    std::scoped_lock lock(mutex_);
    const std::string prefix = dir.empty() ? std::string() : std::string(dir) + "/";
    std::vector<core::vfs::FileInfo> out;
    std::set<std::string> directories;
    for (const ArchiveEntry& entry : reader_.entries()) {
        if (!entry.name.starts_with(prefix)) {
            continue;
        }
        const std::string rest = entry.name.substr(prefix.size());
        // Every directory between `dir` and the entry (only the first level unless recursive).
        for (std::size_t slash = rest.find('/'); slash != std::string::npos; slash = rest.find('/', slash + 1)) {
            directories.insert(prefix + rest.substr(0, slash));
            if (!recursive) {
                break;
            }
        }
        if (recursive || rest.find('/') == std::string::npos) {
            out.push_back({entry.name, entry.size, false});
        }
    }
    for (const std::string& d : directories) {
        out.push_back({d, 0, true});
    }
    std::sort(out.begin(), out.end(),
              [](const core::vfs::FileInfo& a, const core::vfs::FileInfo& b) { return a.path < b.path; });
    return out;
}

} // namespace f3d::io
