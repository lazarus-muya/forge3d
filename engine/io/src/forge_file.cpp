// SPDX-License-Identifier: Apache-2.0
#include "f3d/io/forge_file.hpp"
#include "f3d/io/world_chunks.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/version.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <cstdio>
#include <exception>
#include <system_error>

#if defined(_WIN32)
#include <io.h> // _commit
#else
#include <unistd.h> // fsync
#endif

namespace f3d::io {
namespace {

constexpr const char* kManifest = "manifest.json";
constexpr const char* kScene = "scene.json";
constexpr const char* kEditor = "editor.json";
constexpr const char* kChunkPrefix = "chunks/";

ForgeResult failure(std::string message) {
    ForgeResult r;
    r.error = std::move(message);
    return r;
}

/// Flushes the C buffers and the OS cache of `file` to the disk.
bool flush_to_disk(std::FILE* file) {
    if (std::fflush(file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

nlohmann::json chunk_row(const ArchiveEntry& entry, const char* kind) {
    char crc[9];
    std::snprintf(crc, sizeof(crc), "%08x", entry.crc);
    return {{"name", entry.name},
            {"kind", kind},
            {"compression", std::string(to_string(entry.compression))},
            {"size", entry.size},
            {"stored_size", entry.stored_size},
            {"crc32", crc}};
}

} // namespace

// --- Migrations ------------------------------------------------------------------------------

void MigrationChain::add(int from_version, MigrationStep step) {
    steps_[from_version] = std::move(step);
}

bool MigrationChain::migrate(ForgeDocument& document, int target, std::string& error) const {
    if (document.version > target) {
        error = "file format version " + std::to_string(document.version) + " is newer than this build supports (" +
                std::to_string(target) + ")";
        return false;
    }
    while (document.version < target) {
        const auto it = steps_.find(document.version);
        if (it == steps_.end()) {
            error = "no migration from format version " + std::to_string(document.version);
            return false;
        }
        std::string step_error;
        if (!it->second(document, step_error)) {
            error = "migration v" + std::to_string(document.version) + " -> v" + std::to_string(document.version + 1) +
                    " failed: " + step_error;
            return false;
        }
        ++document.version;
        document.manifest["format_version"] = document.version;
    }
    return true;
}

const MigrationChain& forge_migrations() {
    // Version 1 is the first format; future steps are added here as
    // chain.add(1, migrate_v1_to_v2), with a fixture of the old version in tests/data/forge/.
    static const MigrationChain chain;
    return chain;
}

// --- Save ------------------------------------------------------------------------------------

ForgeResult save_forge(const std::filesystem::path& path, const scene::World& world, const ForgeSaveOptions& options) {
    F3D_ZONE;
    std::filesystem::path temp = path;
    temp += ".tmp";
    std::FILE* file = open_file(temp, "wb");
    if (!file) {
        return failure("cannot create " + temp.string());
    }
    const auto abandon = [&](const std::string& why) {
        std::fclose(file);
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return failure(path.string() + ": " + why);
    };

    ArchiveWriter writer(file);
    nlohmann::json chunks = nlohmann::json::array();
    const std::string scene_text = scene::snapshot_world(world).dump();
    if (!writer.add(kScene, scene_text, options.compression)) {
        return abandon(writer.error());
    }
    chunks.push_back(chunk_row(writer.entries().back(), "scene"));
    if (!options.editor.is_null() && !options.editor.empty()) {
        if (!writer.add(kEditor, options.editor.dump(), options.compression)) {
            return abandon(writer.error());
        }
        chunks.push_back(chunk_row(writer.entries().back(), "editor"));
    }
    // Terrain heights and weights, scatter painting (world_chunks.hpp), then the caller's.
    std::map<std::string, Bytes> all_chunks = world_chunks(const_cast<scene::World&>(world));
    for (const auto& [name, data] : options.chunks) {
        all_chunks[name] = data;
    }
    for (const auto& [name, data] : all_chunks) {
        if (!writer.add(kChunkPrefix + name, data, options.compression)) {
            return abandon(writer.error());
        }
        chunks.push_back(chunk_row(writer.entries().back(), "binary"));
    }
    const nlohmann::json manifest = {{"format", kForgeFormatName},
                                     {"format_version", kForgeFormatVersion},
                                     {"app_version", std::string(core::version())},
                                     {"chunks", std::move(chunks)}};
    if (!writer.add(kManifest, manifest.dump(2), Compression::Store) || !writer.finish()) {
        return abandon(writer.error());
    }
    if (!flush_to_disk(file)) {
        return abandon("cannot flush to disk");
    }
    if (std::fclose(file) != 0) {
        return failure("cannot close " + temp.string());
    }
    if (options.fail_before_rename) {
        return failure("simulated failure before rename (test hook)");
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec); // replaces an existing target
    if (ec) {
        return failure("cannot replace " + path.string() + ": " + ec.message());
    }
    ForgeResult r;
    r.ok = true;
    return r;
}

// --- Load ------------------------------------------------------------------------------------

ForgeResult read_forge(const std::filesystem::path& path, ForgeDocument& document, const MigrationChain& migrations) {
    F3D_ZONE;
    ArchiveReader reader(path);
    if (!reader.ok()) {
        return failure(path.string() + ": " + reader.error());
    }
    const auto text = reader.read_text(kManifest);
    if (!text) {
        return failure(path.string() + ": " + reader.error());
    }
    ForgeResult result;
    try {
        document = ForgeDocument{};
        document.manifest = nlohmann::json::parse(*text);
        if (document.manifest.value("format", std::string()) != kForgeFormatName) {
            return failure(path.string() + ": not a forge3d project");
        }
        document.version = document.manifest.value("format_version", 0);
        result.loaded_version = document.version;
        if (!document.manifest.contains("chunks") || !document.manifest.at("chunks").is_array()) {
            return failure(path.string() + ": manifest has no chunk table");
        }
        // Every chunk listed must be present and intact (read() verifies size and CRC).
        for (const auto& row : document.manifest.at("chunks")) {
            const std::string name = row.at("name").get<std::string>();
            const ArchiveEntry* entry = reader.find(name);
            if (!entry || entry->size != row.value("size", std::uint64_t{0})) {
                return failure(path.string() + ": chunk '" + name + "' is missing or has the wrong size");
            }
            std::optional<Bytes> data = reader.read(name);
            if (!data) {
                return failure(path.string() + ": " + reader.error());
            }
            if (name == kScene) {
                document.scene = nlohmann::json::parse(data->begin(), data->end());
            } else if (name == kEditor) {
                document.editor = nlohmann::json::parse(data->begin(), data->end());
            } else if (name.starts_with(kChunkPrefix)) {
                document.chunks.emplace(name.substr(std::char_traits<char>::length(kChunkPrefix)), std::move(*data));
            }
        }
        if (!reader.find(kScene)) {
            return failure(path.string() + ": no scene.json");
        }
    } catch (const std::exception& e) {
        return failure(path.string() + ": " + e.what());
    }
    std::string error;
    if (!migrations.migrate(document, kForgeFormatVersion, error)) {
        return failure(path.string() + ": " + error);
    }
    result.ok = true;
    result.editor = document.editor;
    return result;
}

ForgeResult load_forge(const std::filesystem::path& path, scene::World& world, const MigrationChain& migrations) {
    F3D_ZONE;
    ForgeDocument document;
    ForgeResult result = read_forge(path, document, migrations);
    if (!result.ok) {
        world.clear();
        return result;
    }
    std::string error;
    if (!scene::restore_world(world, document.scene, &error)) {
        world.clear();
        return failure(path.string() + ": " + error);
    }
    restore_world_chunks(world, document.chunks);
    return result;
}

std::filesystem::path autosave_path(const std::filesystem::path& project) {
    std::filesystem::path out = project;
    out.replace_extension(); // drop .forge
    out += ".autosave.forge";
    return out;
}

bool autosave_is_newer(const std::filesystem::path& project, const std::filesystem::path& autosave) {
    std::error_code ec;
    if (!std::filesystem::exists(autosave, ec)) {
        return false;
    }
    if (!std::filesystem::exists(project, ec)) {
        return true;
    }
    const auto a = std::filesystem::last_write_time(autosave, ec);
    const auto p = std::filesystem::last_write_time(project, ec);
    return !ec && a > p;
}

} // namespace f3d::io
