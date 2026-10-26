// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/asset_database.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>

namespace f3d::assets {
namespace {

constexpr const char* kMetaFormat = "forge3d.meta";
constexpr std::string_view kMetaSuffix = ".meta";

std::string lower_extension(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    std::string ext(path.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// Files and directories the scan skips: hidden ones (".git", ".forge3d" caches)
/// and temporary files.
bool ignored(std::string_view relative) {
    std::size_t start = 0;
    while (start < relative.size()) {
        const auto end = relative.find('/', start);
        const std::string_view segment = relative.substr(start, end - start);
        if (!segment.empty() && segment.front() == '.') {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return relative.ends_with(".tmp");
}

std::string_view relative_part(std::string_view vfs_path) {
    const auto sep = vfs_path.find("://");
    return sep == std::string_view::npos ? vfs_path : vfs_path.substr(sep + 3);
}

} // namespace

std::string meta_to_json(const AssetMeta& meta) {
    nlohmann::json j = {{"format", kMetaFormat},         {"version", AssetMeta::kVersion},
                        {"guid", meta.guid.to_string()}, {"importer", meta.importer},
                        {"settings", meta.settings},     {"source_hash", meta.source_hash}};
    if (!meta.tags.empty()) {
        j["tags"] = meta.tags;
    }
    if (!meta.dependencies.empty()) {
        j["dependencies"] = meta.dependencies;
    }
    return j.dump(2) + "\n";
}

std::optional<AssetMeta> parse_meta(std::string_view text, std::string* error) {
    const auto fail = [&](const char* why) -> std::optional<AssetMeta> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return fail("not valid JSON");
    }
    if (j.value("format", std::string()) != kMetaFormat) {
        return fail("not a forge3d.meta file");
    }
    if (j.value("version", 0) < 1 || j.value("version", 0) > AssetMeta::kVersion) {
        return fail("unsupported .meta version");
    }
    AssetMeta meta;
    const auto guid = core::Guid::parse(j.value("guid", std::string()));
    if (!guid || guid->is_null()) {
        return fail("missing or invalid guid");
    }
    meta.guid = *guid;
    meta.importer = j.value("importer", std::string());
    meta.settings = j.value("settings", nlohmann::json::object());
    meta.source_hash = j.value("source_hash", std::string());
    const auto strings = [&](const char* key, std::vector<std::string>& out) {
        if (j.contains(key) && j.at(key).is_array()) {
            for (const auto& item : j.at(key)) {
                if (item.is_string()) {
                    out.push_back(item.get<std::string>());
                }
            }
        }
    };
    strings("tags", meta.tags);
    strings("dependencies", meta.dependencies);
    return meta;
}

std::string_view importer_for(std::string_view path) {
    const std::string ext = lower_extension(path);
    if (ext == ".gltf" || ext == ".glb") {
        return "gltf";
    }
    if (ext == ".fbx") {
        return "fbx";
    }
    if (ext == ".obj") {
        return "obj";
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
        return "texture";
    }
    if (ext == ".exr" || ext == ".hdr") {
        return "texture_hdr";
    }
    if (ext == ".f3dprefab") {
        return "prefab";
    }
    if (ext == ".f3dmat") {
        return "material";
    }
    return {};
}

std::string hash_source(std::span<const std::uint8_t> data) {
    std::uint64_t h = 0xcbf29ce484222325ull; // FNV-1a 64
    for (const std::uint8_t byte : data) {
        h = (h ^ byte) * 0x100000001b3ull;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "fnv1a64:%016llx", static_cast<unsigned long long>(h));
    return text;
}

ScanResult AssetDatabase::scan(core::vfs::FileSystem& fs, std::string_view root, const AssetDatabase* previous) {
    F3D_ZONE;
    ScanResult result;
    ScanReport& report = result.report;
    const std::vector<core::vfs::FileInfo> files = fs.list(root, true);

    std::unordered_set<std::string> present;
    for (const core::vfs::FileInfo& f : files) {
        if (!f.directory) {
            present.insert(f.path);
        }
    }
    std::unordered_set<core::Guid> seen;
    for (const core::vfs::FileInfo& file : files) {
        if (file.directory || ignored(relative_part(file.path))) {
            continue;
        }
        if (file.path.ends_with(kMetaSuffix)) {
            const std::string source = file.path.substr(0, file.path.size() - kMetaSuffix.size());
            if (!present.contains(source)) {
                report.orphaned.push_back(file.path);
            }
            continue;
        }
        const std::string_view importer = importer_for(file.path);
        if (importer.empty()) {
            continue;
        }
        ++report.sources;
        AssetRecord record;
        record.path = file.path;
        record.importer = std::string(importer);
        record.size = file.size;
        record.modified = file.modified;

        // The source hash: reuse the previous scan's when the file looks unchanged.
        const AssetRecord* old = previous ? previous->find_path(file.path) : nullptr;
        if (old && old->size == file.size && old->modified == file.modified && file.modified != 0) {
            record.source_hash = old->source_hash;
        } else if (const auto data = fs.read(file.path)) {
            record.source_hash = hash_source(*data);
            ++report.hashed;
        } else {
            report.errors.push_back(file.path + ": cannot read");
            continue;
        }

        // The .meta: read it, or create it on first sight.
        const std::string meta_path = file.path + std::string(kMetaSuffix);
        std::optional<AssetMeta> meta;
        bool rewrite = false;
        if (present.contains(meta_path)) {
            std::string error;
            if (const auto text = fs.read_text(meta_path)) {
                meta = parse_meta(*text, &error);
            }
            if (!meta) {
                report.errors.push_back(meta_path + ": " + (error.empty() ? "cannot read" : error) + " (regenerated)");
                report.regenerated.push_back(file.path);
                rewrite = true;
            } else if (seen.contains(meta->guid)) {
                // A copy of another asset (with its .meta): it is a new asset.
                report.regenerated.push_back(file.path);
                meta->guid = core::Guid::generate();
                meta->source_hash.clear(); // never imported under this identity
                rewrite = true;
            }
        } else {
            ++report.metas_created;
            rewrite = true;
        }
        if (!meta) {
            meta = AssetMeta{};
            meta->guid = core::Guid::generate();
            meta->importer = record.importer;
        }
        if (rewrite && !fs.write_text(meta_path, meta_to_json(*meta))) {
            report.errors.push_back(meta_path + ": cannot write");
        }
        seen.insert(meta->guid);
        record.guid = meta->guid;
        record.settings = meta->settings;
        record.imported_hash = meta->source_hash;
        record.tags = meta->tags;
        record.dependencies = meta->dependencies;
        result.records.push_back(std::move(record));
    }
    std::sort(result.records.begin(), result.records.end(),
              [](const AssetRecord& a, const AssetRecord& b) { return a.path < b.path; });
    return result;
}

void AssetDatabase::apply(ScanResult result) {
    records_ = std::move(result.records);
    report_ = std::move(result.report);
    ++version_;
    by_guid_.clear();
    by_path_.clear();
    for (std::size_t i = 0; i < records_.size(); ++i) {
        by_guid_.emplace(records_[i].guid, i);
        by_path_.emplace(records_[i].path, i);
    }
}

void AssetDatabase::seed(std::vector<AssetRecord> records) {
    ScanResult result;
    result.records = std::move(records);
    std::sort(result.records.begin(), result.records.end(),
              [](const AssetRecord& a, const AssetRecord& b) { return a.path < b.path; });
    apply(std::move(result));
}

std::string_view AssetRecord::type() const noexcept {
    if (importer == "prefab") {
        return "prefab";
    }
    if (importer == "material") {
        return "material";
    }
    return importer.starts_with("texture") ? "texture" : "model";
}

std::string AssetRecord::name() const {
    const auto slash = path.find_last_of('/');
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

ScanReport AssetDatabase::rescan(core::vfs::FileSystem& fs, std::string_view root) {
    apply(scan(fs, root, this));
    return report_;
}

const AssetRecord* AssetDatabase::find(const core::Guid& guid) const {
    const auto it = by_guid_.find(guid);
    return it == by_guid_.end() ? nullptr : &records_[it->second];
}

const AssetRecord* AssetDatabase::find_path(std::string_view path) const {
    const auto it = by_path_.find(std::string(path));
    return it == by_path_.end() ? nullptr : &records_[it->second];
}

void AssetDatabase::clear() {
    ++version_;
    records_.clear();
    by_guid_.clear();
    by_path_.clear();
    report_ = {};
}

} // namespace f3d::assets
