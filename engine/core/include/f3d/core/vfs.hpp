// SPDX-License-Identifier: Apache-2.0
#pragma once

// Virtual file system (phases.md P3.1). Paths are "<mount>://<relative path>",
// e.g. "project://models/rock.glb" or "engine://shaders/grid.ps.hlsl". The
// relative part uses '/' separators and may not escape its mount ("..").
//
// Backends: a directory (read/write) here in core; a .forge archive
// (read-only) in f3d::io (io::ArchiveBackend). Thread-safe: mounts are guarded
// by a mutex and backends are called without holding it.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f3d::core::vfs {

using Bytes = std::vector<std::uint8_t>;

struct FileInfo {
    std::string path; ///< relative to the mount, '/' separators
    std::uint64_t size = 0;
    bool directory = false;
    std::int64_t modified = 0; ///< backend-specific modification stamp (0: unknown); compare for equality only
};

/// Storage behind a mount point. Paths are relative and already validated.
class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
    [[nodiscard]] virtual std::optional<Bytes> read(std::string_view path) const = 0;
    /// Entries directly under `dir` ("" = the root), or the whole tree with `recursive`.
    [[nodiscard]] virtual std::vector<FileInfo> list(std::string_view dir, bool recursive) const = 0;
    [[nodiscard]] virtual bool writable() const { return false; }
    virtual bool write(std::string_view path, const Bytes& data);
    virtual bool remove(std::string_view path);
    /// The OS path of a file, if the backend has one (directory backends).
    [[nodiscard]] virtual std::optional<std::filesystem::path> native_path(std::string_view path) const;
};

/// A directory on disk. Writes create parent directories and are atomic (temp + rename).
class DirectoryBackend final : public Backend {
public:
    explicit DirectoryBackend(std::filesystem::path root, bool writable = true);

    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] std::optional<Bytes> read(std::string_view path) const override;
    [[nodiscard]] std::vector<FileInfo> list(std::string_view dir, bool recursive) const override;
    [[nodiscard]] bool writable() const override { return writable_; }
    bool write(std::string_view path, const Bytes& data) override;
    bool remove(std::string_view path) override;
    [[nodiscard]] std::optional<std::filesystem::path> native_path(std::string_view path) const override;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    bool writable_;
};

/// A parsed VFS path.
struct VPath {
    std::string mount;    ///< "project"
    std::string relative; ///< "models/rock.glb" (normalised; may be empty for the root)
};

/// Splits and normalises "<mount>://<path>": backslashes become '/', "." and
/// empty segments are dropped, ".." that would leave the mount is rejected.
[[nodiscard]] std::optional<VPath> parse(std::string_view path);
/// "<mount>://<relative>".
[[nodiscard]] std::string join(std::string_view mount, std::string_view relative);

class FileSystem {
public:
    /// Mounts `backend` at `mount` ("project", "engine", ...), replacing any previous one.
    void mount(std::string mount, std::shared_ptr<Backend> backend);
    void unmount(std::string_view mount);
    [[nodiscard]] bool mounted(std::string_view mount) const;
    [[nodiscard]] std::shared_ptr<Backend> backend(std::string_view mount) const;

    [[nodiscard]] bool exists(std::string_view path) const;
    [[nodiscard]] std::optional<Bytes> read(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> read_text(std::string_view path) const;
    /// Lists a directory; returned paths are full VFS paths ("project://a/b.png").
    [[nodiscard]] std::vector<FileInfo> list(std::string_view dir, bool recursive = false) const;
    bool write(std::string_view path, const Bytes& data);
    bool write_text(std::string_view path, std::string_view text);
    bool remove(std::string_view path);
    [[nodiscard]] std::optional<std::filesystem::path> native_path(std::string_view path) const;
    /// The VFS path of an OS path inside a mounted directory (longest match), if any.
    [[nodiscard]] std::optional<std::string> to_virtual(const std::filesystem::path& native) const;

private:
    [[nodiscard]] std::shared_ptr<Backend> resolve(std::string_view path, VPath& parsed) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Backend>> mounts_;
};

} // namespace f3d::core::vfs
