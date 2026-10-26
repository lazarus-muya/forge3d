// SPDX-License-Identifier: Apache-2.0
#pragma once

// .forge project files, format version 1 (phases.md P2.6, docs/format.md).
//
//   scene.json      the scene: scene::snapshot_world (entities, layers, bookmarks)
//   editor.json     optional editor state (camera), not part of the scene
//   chunks/<name>   binary payloads (terrain tiles, ... from M6 on)
//   manifest.json   format, format_version, app_version and the chunk table;
//                   stored uncompressed and written last
//
// Loading runs the migration chain (v_n -> v_n+1 ...) up to the current
// version before the scene is restored. Saving is atomic: everything is written
// to "<file>.tmp", flushed to disk, then renamed over the target.

#include "f3d/io/archive.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::io {

/// The format version this build writes.
inline constexpr int kForgeFormatVersion = 1;
/// The manifest's "format" value.
inline constexpr const char* kForgeFormatName = "forge3d.project";

/// A .forge file's contents, as migrations see them.
struct ForgeDocument {
    int version = kForgeFormatVersion;
    nlohmann::json manifest = nlohmann::json::object();
    nlohmann::json scene = nlohmann::json::object();
    nlohmann::json editor = nlohmann::json::object(); ///< empty if the file has none
    std::map<std::string, Bytes> chunks;              ///< by name, without the "chunks/" prefix
};

/// Upgrades a document from one format version to the next. Returns false and
/// sets `error` if it can't.
using MigrationStep = std::function<bool(ForgeDocument& document, std::string& error)>;

/// The chain of migrations, one per version step.
class MigrationChain {
public:
    /// Registers the step from `from_version` to `from_version + 1`.
    void add(int from_version, MigrationStep step);
    /// Applies steps until `document.version == target`. Fails for a missing step
    /// or a document newer than `target`.
    bool migrate(ForgeDocument& document, int target, std::string& error) const;
    [[nodiscard]] bool empty() const noexcept { return steps_.empty(); }

private:
    std::map<int, MigrationStep> steps_;
};

/// The migrations this build knows (empty while version 1 is the only one).
[[nodiscard]] const MigrationChain& forge_migrations();

struct ForgeSaveOptions {
    nlohmann::json editor;               ///< written as editor.json unless null/empty
    std::map<std::string, Bytes> chunks; ///< written as chunks/<name>
    Compression compression = Compression::Zstd;
    /// Test hook: stop after writing the temporary file, before the rename, as if
    /// the process died there. The target is left untouched.
    bool fail_before_rename = false;
};

struct ForgeResult {
    bool ok = false;
    std::string error;
    int loaded_version = 0; ///< load: the version found in the file (before migration)
    nlohmann::json editor;  ///< load: editor.json, or null
};

/// Saves `world` to `path` atomically.
ForgeResult save_forge(const std::filesystem::path& path, const scene::World& world,
                       const ForgeSaveOptions& options = {});

/// Reads (and migrates) a .forge file without touching a scene.
ForgeResult read_forge(const std::filesystem::path& path, ForgeDocument& document,
                       const MigrationChain& migrations = forge_migrations());

/// Loads a .forge file into `world` (replacing its contents). On failure the
/// world is left empty.
ForgeResult load_forge(const std::filesystem::path& path, scene::World& world,
                       const MigrationChain& migrations = forge_migrations());

/// "<dir>/<name>.autosave.forge" for "<dir>/<name>.forge".
[[nodiscard]] std::filesystem::path autosave_path(const std::filesystem::path& project);
/// True if `autosave` exists and was written after `project` (or `project` doesn't exist).
[[nodiscard]] bool autosave_is_newer(const std::filesystem::path& project, const std::filesystem::path& autosave);

} // namespace f3d::io
