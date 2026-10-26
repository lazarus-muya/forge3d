// SPDX-License-Identifier: Apache-2.0
#pragma once

// Asset identity (phases.md P3.1). Every source file the asset pipeline knows
// (models, textures) gets a "<source>.meta" sidecar the first time it is seen:
//
//   { "format": "forge3d.meta", "version": 1, "guid": "...", "importer": "gltf",
//     "settings": { ... }, "source_hash": "fnv1a64:..." }
//
// The GUID is the asset's identity: it stays the same when the source is renamed
// or moved together with its .meta, so references (AssetRef::guid) survive.
// AssetDatabase maps GUIDs to VFS paths; scanning reports orphaned .meta files
// (source gone) and GUID collisions (a file copied with its .meta).

#include "f3d/core/guid.hpp"
#include "f3d/core/vfs.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f3d::assets {

/// The sidecar's contents.
struct AssetMeta {
    static constexpr int kVersion = 1;
    core::Guid guid;
    std::string importer;                               ///< "gltf", "fbx", "obj", "texture", "texture_hdr"
    nlohmann::json settings = nlohmann::json::object(); ///< importer settings
    std::string source_hash;                            ///< hash of the source when it was last imported ("" = never)
    std::vector<std::string> tags;                      ///< user tags (asset browser filter)
    std::vector<std::string> dependencies;              ///< VFS paths of files the asset uses (found at import)
};

[[nodiscard]] std::string meta_to_json(const AssetMeta& meta);
[[nodiscard]] std::optional<AssetMeta> parse_meta(std::string_view text, std::string* error = nullptr);

/// The importer for a source path ("" if it isn't an asset source), by extension.
[[nodiscard]] std::string_view importer_for(std::string_view path);
/// "fnv1a64:<16 hex digits>" of the bytes.
[[nodiscard]] std::string hash_source(std::span<const std::uint8_t> data);

/// One known asset.
struct AssetRecord {
    core::Guid guid;
    std::string path; ///< VFS path of the source ("project://models/rock.glb")
    std::string importer;
    nlohmann::json settings = nlohmann::json::object();
    std::string source_hash;   ///< the source as it is now
    std::string imported_hash; ///< the source when it was last imported (from the .meta)
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    std::vector<std::string> tags;
    std::vector<std::string> dependencies; ///< VFS paths

    /// "model", "texture", "prefab" or "material".
    [[nodiscard]] std::string_view type() const noexcept;
    /// File name without directory and extension.
    [[nodiscard]] std::string name() const;
    /// True if the source changed since its last import (or was never imported).
    [[nodiscard]] bool stale() const noexcept { return imported_hash != source_hash; }
};

struct ScanReport {
    std::size_t sources = 0;
    std::size_t metas_created = 0;
    std::size_t hashed = 0;               ///< sources read and hashed (others were unchanged)
    std::vector<std::string> orphaned;    ///< .meta files whose source is gone
    std::vector<std::string> regenerated; ///< sources whose .meta had a duplicate or invalid GUID (new GUID written)
    std::vector<std::string> errors;
};

/// The result of a scan, applied to a database on the main thread.
struct ScanResult {
    std::vector<AssetRecord> records;
    ScanReport report;
};

class AssetDatabase {
public:
    /// Scans `root` (a VFS directory, e.g. "project://") recursively. Creates
    /// missing .meta files (and fixes invalid or duplicate ones) through `fs`.
    /// Files whose size and modification stamp match `previous` aren't re-read.
    /// Thread-safe with respect to the database (doesn't touch it): run it on a
    /// worker and apply() the result on the main thread.
    [[nodiscard]] static ScanResult scan(core::vfs::FileSystem& fs, std::string_view root,
                                         const AssetDatabase* previous = nullptr);

    /// Replaces the database's contents with a scan result.
    void apply(ScanResult result);
    /// Seeds the database from records (e.g. the persistent index), so the next
    /// scan only re-hashes files whose size or modification stamp changed.
    void seed(std::vector<AssetRecord> records);
    /// scan + apply.
    ScanReport rescan(core::vfs::FileSystem& fs, std::string_view root);

    [[nodiscard]] const AssetRecord* find(const core::Guid& guid) const;
    [[nodiscard]] const AssetRecord* find_path(std::string_view path) const;
    [[nodiscard]] const std::vector<AssetRecord>& records() const noexcept { return records_; }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] const ScanReport& last_report() const noexcept { return report_; }
    /// Increments whenever the contents change (views refresh their caches).
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
    void clear();

private:
    std::vector<AssetRecord> records_; ///< sorted by path
    std::unordered_map<core::Guid, std::size_t> by_guid_;
    std::unordered_map<std::string, std::size_t> by_path_;
    ScanReport report_;
    std::uint64_t version_ = 0;
};

} // namespace f3d::assets
