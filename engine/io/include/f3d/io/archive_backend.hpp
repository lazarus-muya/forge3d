// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/vfs.hpp"
#include "f3d/io/archive.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace f3d::io {

/// A read-only VFS backend over an archive (a .forge file): mount it to read
/// its entries as files, e.g. "project://scene.json", "project://chunks/...".
/// Directories are implied by the entry names.
class ArchiveBackend final : public core::vfs::Backend {
public:
    /// Opens `path`; check ok() / error().
    explicit ArchiveBackend(const std::filesystem::path& path);

    [[nodiscard]] bool ok() const noexcept { return reader_.ok(); }
    [[nodiscard]] const std::string& error() const noexcept { return reader_.error(); }

    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] std::optional<core::vfs::Bytes> read(std::string_view path) const override;
    [[nodiscard]] std::vector<core::vfs::FileInfo> list(std::string_view dir, bool recursive) const override;

private:
    mutable std::mutex mutex_; ///< the reader seeks a shared FILE*
    mutable ArchiveReader reader_;
};

} // namespace f3d::io
