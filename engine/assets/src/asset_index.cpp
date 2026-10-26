// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/asset_index.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace f3d::assets {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS info(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS assets(
    guid TEXT PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    type TEXT NOT NULL,
    importer TEXT NOT NULL,
    source_hash TEXT NOT NULL,
    imported_hash TEXT NOT NULL,
    size INTEGER NOT NULL,
    modified INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS assets_name ON assets(name COLLATE NOCASE);
CREATE TABLE IF NOT EXISTS tags(guid TEXT NOT NULL, tag TEXT NOT NULL, PRIMARY KEY(guid, tag));
CREATE INDEX IF NOT EXISTS tags_tag ON tags(tag);
CREATE TABLE IF NOT EXISTS dependencies(guid TEXT NOT NULL, path TEXT NOT NULL, PRIMARY KEY(guid, path));
CREATE INDEX IF NOT EXISTS dependencies_path ON dependencies(path);
CREATE TABLE IF NOT EXISTS thumbnails(guid TEXT PRIMARY KEY, path TEXT NOT NULL, source_hash TEXT NOT NULL);
)sql";

/// RAII prepared statement.
class Statement {
public:
    Statement(::sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            F3D_LOG_ERROR("asset index: {} ({})", sqlite3_errmsg(db), sql);
            stmt_ = nullptr;
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int i, std::string_view text) {
        sqlite3_bind_text(stmt_, i, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& bind(int i, std::int64_t value) {
        sqlite3_bind_int64(stmt_, i, value);
        return *this;
    }
    bool step() { return stmt_ && sqlite3_step(stmt_) == SQLITE_ROW; }
    bool run() {
        const int rc = stmt_ ? sqlite3_step(stmt_) : SQLITE_ERROR;
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
        return rc == SQLITE_DONE || rc == SQLITE_ROW;
    }
    std::string text(int column) const {
        const auto* p = sqlite3_column_text(stmt_, column);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(stmt_, column); }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

core::Guid parse_guid(const std::string& text) {
    return core::Guid::parse(text).value_or(core::Guid{});
}

/// LIKE pattern for a substring, with the wildcard characters escaped.
std::string like_pattern(std::string_view text) {
    std::string out = "%";
    for (const char c : text) {
        if (c == '%' || c == '_' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('%');
    return out;
}

} // namespace

std::unique_ptr<AssetIndex> AssetIndex::open(const std::filesystem::path& file, std::string* error) {
    F3D_ZONE;
    if (file != ":memory:") {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    const std::u8string utf8 = file.u8string();
    ::sqlite3* db = nullptr;
    if (sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        if (error) {
            *error = db ? sqlite3_errmsg(db) : "cannot open the asset index";
        }
        sqlite3_close(db);
        return nullptr;
    }
    std::unique_ptr<AssetIndex> index(new AssetIndex(db));
    index->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=OFF;");
    if (!index->exec(kSchema, error)) {
        return nullptr;
    }
    // A different schema version: it's a cache, so start over.
    std::string version;
    {
        Statement s(db, "SELECT value FROM info WHERE key = 'schema'");
        if (s.step()) {
            version = s.text(0);
        }
    }
    if (version != std::to_string(kSchemaVersion)) {
        index->exec("DELETE FROM assets; DELETE FROM tags; DELETE FROM dependencies; DELETE FROM thumbnails;");
        Statement s(db, "INSERT OR REPLACE INTO info(key, value) VALUES('schema', ?)");
        s.bind(1, std::to_string(kSchemaVersion)).run();
    }
    return index;
}

AssetIndex::~AssetIndex() {
    sqlite3_close(db_);
}

bool AssetIndex::exec(const char* sql, std::string* error) const {
    char* message = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        if (error) {
            *error = message ? message : "SQL error";
        }
        F3D_LOG_ERROR("asset index: {}", message ? message : "SQL error");
        sqlite3_free(message);
        return false;
    }
    return true;
}

AssetIndex::SyncStats AssetIndex::sync(const AssetDatabase& database) {
    F3D_ZONE;
    SyncStats stats;
    // What the index holds now: guid -> (path, source hash, imported hash, size, stamp, tags, deps).
    struct Row {
        std::string path, source_hash, imported_hash;
        std::int64_t size = 0, modified = 0;
        std::set<std::string> tags, dependencies;
    };
    std::map<std::string, Row> existing;
    {
        Statement s(db_, "SELECT guid, path, source_hash, imported_hash, size, modified FROM assets");
        while (s.step()) {
            existing[s.text(0)] = {s.text(1), s.text(2), s.text(3), s.integer(4), s.integer(5), {}, {}};
        }
        Statement t(db_, "SELECT guid, tag FROM tags");
        while (t.step()) {
            existing[t.text(0)].tags.insert(t.text(1));
        }
        Statement d(db_, "SELECT guid, path FROM dependencies");
        while (d.step()) {
            existing[d.text(0)].dependencies.insert(d.text(1));
        }
    }

    exec("BEGIN");
    Statement upsert(db_, "INSERT OR REPLACE INTO assets(guid, path, name, type, importer, source_hash, imported_hash, "
                          "size, modified) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
    Statement delete_tags(db_, "DELETE FROM tags WHERE guid = ?");
    Statement insert_tag(db_, "INSERT OR IGNORE INTO tags(guid, tag) VALUES(?, ?)");
    Statement delete_deps(db_, "DELETE FROM dependencies WHERE guid = ?");
    Statement insert_dep(db_, "INSERT OR IGNORE INTO dependencies(guid, path) VALUES(?, ?)");
    std::set<std::string> present;
    for (const AssetRecord& r : database.records()) {
        const std::string guid = r.guid.to_string();
        present.insert(guid);
        const std::set<std::string> tags(r.tags.begin(), r.tags.end());
        const std::set<std::string> deps(r.dependencies.begin(), r.dependencies.end());
        const auto it = existing.find(guid);
        const bool same = it != existing.end() && it->second.path == r.path &&
                          it->second.source_hash == r.source_hash && it->second.imported_hash == r.imported_hash &&
                          it->second.size == static_cast<std::int64_t>(r.size) && it->second.modified == r.modified &&
                          it->second.tags == tags && it->second.dependencies == deps;
        if (same) {
            ++stats.unchanged;
            continue;
        }
        (it == existing.end() ? stats.inserted : stats.updated) += 1;
        // A path taken over by another GUID (copy/regenerated meta): drop the old row first.
        Statement clash(db_, "DELETE FROM assets WHERE path = ? AND guid != ?");
        clash.bind(1, r.path).bind(2, guid).run();
        upsert.bind(1, guid)
            .bind(2, r.path)
            .bind(3, r.name())
            .bind(4, r.type())
            .bind(5, r.importer)
            .bind(6, r.source_hash)
            .bind(7, r.imported_hash)
            .bind(8, static_cast<std::int64_t>(r.size))
            .bind(9, r.modified)
            .run();
        delete_tags.bind(1, guid).run();
        for (const std::string& tag : tags) {
            insert_tag.bind(1, guid).bind(2, tag).run();
        }
        delete_deps.bind(1, guid).run();
        for (const std::string& dep : deps) {
            insert_dep.bind(1, guid).bind(2, dep).run();
        }
    }
    Statement remove_asset(db_, "DELETE FROM assets WHERE guid = ?");
    for (const auto& [guid, row] : existing) {
        if (!present.contains(guid)) {
            ++stats.removed;
            remove_asset.bind(1, guid).run();
            delete_tags.bind(1, guid).run();
            delete_deps.bind(1, guid).run();
            Statement thumb(db_, "DELETE FROM thumbnails WHERE guid = ?");
            thumb.bind(1, guid).run();
        }
    }
    exec("COMMIT");
    return stats;
}

std::vector<AssetRecord> AssetIndex::load_records() const {
    std::map<std::string, AssetRecord> records;
    {
        Statement s(db_, "SELECT guid, path, importer, source_hash, imported_hash, size, modified FROM assets");
        while (s.step()) {
            AssetRecord r;
            r.guid = parse_guid(s.text(0));
            r.path = s.text(1);
            r.importer = s.text(2);
            r.source_hash = s.text(3);
            r.imported_hash = s.text(4);
            r.size = static_cast<std::uint64_t>(s.integer(5));
            r.modified = s.integer(6);
            records[s.text(0)] = std::move(r);
        }
    }
    Statement t(db_, "SELECT guid, tag FROM tags ORDER BY tag");
    while (t.step()) {
        if (const auto it = records.find(t.text(0)); it != records.end()) {
            it->second.tags.push_back(t.text(1));
        }
    }
    Statement d(db_, "SELECT guid, path FROM dependencies ORDER BY path");
    while (d.step()) {
        if (const auto it = records.find(d.text(0)); it != records.end()) {
            it->second.dependencies.push_back(d.text(1));
        }
    }
    std::vector<AssetRecord> out;
    out.reserve(records.size());
    for (auto& [guid, record] : records) {
        out.push_back(std::move(record));
    }
    return out;
}

std::vector<AssetSummary> AssetIndex::search(const AssetQuery& query) const {
    F3D_ZONE;
    std::string sql = "SELECT a.guid, a.path, a.name, a.type, a.source_hash != a.imported_hash FROM assets a WHERE 1";
    if (!query.text.empty()) {
        sql += " AND (a.name LIKE ?1 ESCAPE '\\' OR a.path LIKE ?1 ESCAPE '\\')";
    }
    if (!query.type.empty()) {
        sql += " AND a.type = ?2";
    }
    for (std::size_t i = 0; i < query.tags.size(); ++i) {
        sql += " AND EXISTS (SELECT 1 FROM tags t WHERE t.guid = a.guid AND t.tag = ?" + std::to_string(3 + i) + ")";
    }
    sql += " ORDER BY a.name COLLATE NOCASE, a.path LIMIT " + std::to_string(query.limit);
    Statement s(db_, sql.c_str());
    if (!query.text.empty()) {
        s.bind(1, like_pattern(query.text));
    }
    if (!query.type.empty()) {
        s.bind(2, query.type);
    }
    for (std::size_t i = 0; i < query.tags.size(); ++i) {
        s.bind(static_cast<int>(3 + i), query.tags[i]);
    }
    std::vector<AssetSummary> out;
    while (s.step()) {
        out.push_back({parse_guid(s.text(0)), s.text(1), s.text(2), s.text(3), s.integer(4) != 0});
    }
    return out;
}

std::size_t AssetIndex::size() const {
    Statement s(db_, "SELECT COUNT(*) FROM assets");
    return s.step() ? static_cast<std::size_t>(s.integer(0)) : 0;
}

std::vector<std::string> AssetIndex::tags() const {
    std::vector<std::string> out;
    Statement s(db_, "SELECT DISTINCT tag FROM tags ORDER BY tag");
    while (s.step()) {
        out.push_back(s.text(0));
    }
    return out;
}

std::vector<core::Guid> AssetIndex::dependencies(const core::Guid& guid) const {
    std::vector<core::Guid> out;
    Statement s(db_, "SELECT a.guid FROM dependencies d JOIN assets a ON a.path = d.path WHERE d.guid = ? "
                     "ORDER BY a.path");
    s.bind(1, guid.to_string());
    while (s.step()) {
        out.push_back(parse_guid(s.text(0)));
    }
    return out;
}

std::vector<core::Guid> AssetIndex::dependents(const core::Guid& guid) const {
    std::vector<core::Guid> out;
    Statement s(db_, "SELECT d.guid FROM dependencies d JOIN assets a ON a.path = d.path WHERE a.guid = ? "
                     "ORDER BY d.guid");
    s.bind(1, guid.to_string());
    while (s.step()) {
        out.push_back(parse_guid(s.text(0)));
    }
    return out;
}

void AssetIndex::set_thumbnail(const core::Guid& guid, const ThumbnailInfo& thumbnail) {
    Statement s(db_, "INSERT OR REPLACE INTO thumbnails(guid, path, source_hash) VALUES(?, ?, ?)");
    s.bind(1, guid.to_string()).bind(2, thumbnail.path).bind(3, thumbnail.source_hash).run();
}

std::optional<ThumbnailInfo> AssetIndex::thumbnail(const core::Guid& guid) const {
    Statement s(db_, "SELECT path, source_hash FROM thumbnails WHERE guid = ?");
    s.bind(1, guid.to_string());
    if (!s.step()) {
        return std::nullopt;
    }
    return ThumbnailInfo{s.text(0), s.text(1)};
}

} // namespace f3d::assets
