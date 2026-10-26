// SPDX-License-Identifier: Apache-2.0
#pragma once

// The persistent asset index (phases.md P3.3): a SQLite database in the project
// (".forge3d/index.sqlite") with the assets, their tags and dependencies, and
// the thumbnail cache. It is a cache of the .meta files plus the scan results:
// deleting it loses nothing. It makes the browser's queries fast, and seeds the
// next session's scan so unchanged files aren't re-hashed.

#include "f3d/assets/asset_database.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace f3d::assets {

struct AssetQuery {
    std::string text;              ///< case-insensitive substring of the name or path ("" = all)
    std::string type;              ///< "model", "texture" or "" (all)
    std::vector<std::string> tags; ///< every tag must be present
    std::size_t limit = 1000;
};

struct AssetSummary {
    core::Guid guid;
    std::string path;
    std::string name;
    std::string type;
    bool stale = false;
};

struct ThumbnailInfo {
    std::string path;        ///< VFS path of the cached PNG
    std::string source_hash; ///< the imported source hash it was made from
};

class AssetIndex {
public:
    static constexpr int kSchemaVersion = 1;

    /// Opens (creating if needed) an index file; ":memory:" for an in-memory one.
    /// An index of another schema version is rebuilt (it's only a cache).
    [[nodiscard]] static std::unique_ptr<AssetIndex> open(const std::filesystem::path& file,
                                                          std::string* error = nullptr);
    ~AssetIndex();
    AssetIndex(const AssetIndex&) = delete;
    AssetIndex& operator=(const AssetIndex&) = delete;

    struct SyncStats {
        std::size_t inserted = 0;
        std::size_t updated = 0;
        std::size_t removed = 0;
        std::size_t unchanged = 0;
    };
    /// Brings the index in line with `database` in one transaction, writing only
    /// the rows that changed (incremental).
    SyncStats sync(const AssetDatabase& database);

    /// Every asset as records (path, guid, importer, hashes, size, stamp, tags,
    /// dependencies) for AssetDatabase::seed().
    [[nodiscard]] std::vector<AssetRecord> load_records() const;

    [[nodiscard]] std::vector<AssetSummary> search(const AssetQuery& query) const;
    [[nodiscard]] std::size_t size() const;
    /// Every tag in use, sorted.
    [[nodiscard]] std::vector<std::string> tags() const;
    /// Assets that `guid` depends on, and assets that depend on `guid`.
    [[nodiscard]] std::vector<core::Guid> dependencies(const core::Guid& guid) const;
    [[nodiscard]] std::vector<core::Guid> dependents(const core::Guid& guid) const;

    void set_thumbnail(const core::Guid& guid, const ThumbnailInfo& thumbnail);
    [[nodiscard]] std::optional<ThumbnailInfo> thumbnail(const core::Guid& guid) const;

private:
    explicit AssetIndex(::sqlite3* db) : db_(db) {}
    bool exec(const char* sql, std::string* error = nullptr) const;

    ::sqlite3* db_ = nullptr;
};

} // namespace f3d::assets
