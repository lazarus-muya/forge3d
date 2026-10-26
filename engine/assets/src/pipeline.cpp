// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/pipeline.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <exception>
#include <fstream>
#include <iterator>

namespace f3d::assets {
namespace {

constexpr const char* kModelFormat = "forge3d.model";

std::string extension_of(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return std::string(path.substr(dot));
}

nlohmann::json node_json(const ModelNode& n) {
    return {{"name", n.name},
            {"parent", n.parent},
            {"mesh", n.mesh},
            {"translation", {n.translation.x, n.translation.y, n.translation.z}},
            {"rotation", {n.rotation.w, n.rotation.x, n.rotation.y, n.rotation.z}},
            {"scale", {n.scale.x, n.scale.y, n.scale.z}}};
}

ModelNode node_from_json(const nlohmann::json& j) {
    ModelNode n;
    n.name = j.value("name", std::string());
    n.parent = j.value("parent", -1);
    n.mesh = j.value("mesh", -1);
    const auto& t = j.at("translation");
    const auto& r = j.at("rotation");
    const auto& s = j.at("scale");
    n.translation = {t[0].get<float>(), t[1].get<float>(), t[2].get<float>()};
    n.rotation = glm::quat(r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>());
    n.scale = {s[0].get<float>(), s[1].get<float>(), s[2].get<float>()};
    return n;
}

/// Rewrites the .meta with the settings used and the imported source hash.
bool record_import(core::vfs::FileSystem& fs, const AssetRecord& asset, const nlohmann::json& settings,
                   const std::string& hash, std::vector<std::string> dependencies = {}) {
    AssetMeta meta;
    meta.guid = asset.guid;
    meta.importer = asset.importer;
    meta.settings = settings;
    meta.source_hash = hash;
    meta.tags = asset.tags;
    meta.dependencies = std::move(dependencies);
    return fs.write_text(asset.path + ".meta", meta_to_json(meta));
}

/// The VFS directory of a VFS path ("project://a/b.obj" -> "project://a/").
std::string directory_of(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return std::string(path.substr(0, slash == std::string_view::npos ? 0 : slash + 1));
}

void report(const ImportProgress& progress, float value) {
    if (progress) {
        progress(value);
    }
}

bool cancelled(const core::jobs::CancellationToken* token) {
    return token && token->cancelled();
}

ImportOutcome import_model(core::vfs::FileSystem& fs, const AssetRecord& asset,
                           const core::jobs::CancellationToken* cancel, const ImportProgress& progress,
                           const std::string& hash) {
    ImportOutcome out;
    const auto native = fs.native_path(asset.path);
    if (!native) {
        out.error = asset.path + ": models must be on disk (a directory mount)";
        return out;
    }
    const MeshCookSettings settings = MeshCookSettings::from_json(asset.settings);
    ModelLoadResult loaded = load_model(*native);
    if (!loaded.scene) {
        out.error = loaded.error;
        return out;
    }
    out.warnings = loaded.scene->warnings;
    report(progress, 0.3f);
    const std::string dir = cooked_dir(asset.guid);
    // Version 2 (P4.2): materials and their images.
    nlohmann::json model = {{"format", kModelFormat}, {"version", 2}, {"name", loaded.scene->name}};
    model["nodes"] = nlohmann::json::array();
    for (const ModelNode& node : loaded.scene->nodes) {
        model["nodes"].push_back(node_json(node));
    }
    model["meshes"] = nlohmann::json::array();
    const std::size_t count = loaded.scene->meshes.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (cancelled(cancel)) {
            out.cancelled = true;
            out.error = "cancelled";
            return out;
        }
        const CookedMesh cooked = cook_mesh(std::move(loaded.scene->meshes[i]), settings);
        const std::string file = "mesh_" + std::to_string(i) + ".f3dmesh";
        if (!fs.write(dir + "/" + file, write_f3dmesh(cooked))) {
            out.error = "cannot write " + dir + "/" + file;
            return out;
        }
        out.outputs.push_back(dir + "/" + file);
        model["meshes"].push_back({{"file", file},
                                   {"name", cooked.name},
                                   {"lods", cooked.lods.size()},
                                   {"triangles", cooked.triangle_count(0)}});
        report(progress, 0.3f + 0.4f * static_cast<float>(i + 1) / static_cast<float>(count));
    }
    model["materials"] = nlohmann::json::array();
    for (const Material& material : loaded.scene->materials) {
        model["materials"].push_back(material_to_json(material));
    }
    // Images: cooked with the kind of the slot that uses them. An image that can't be
    // decoded is a warning (the material falls back to its factors).
    const std::size_t images = loaded.scene->images.size();
    for (std::size_t i = 0; i < images; ++i) {
        if (cancelled(cancel)) {
            out.cancelled = true;
            out.error = "cancelled";
            return out;
        }
        const ModelImage& image = loaded.scene->images[i];
        std::string error;
        const auto source = decode_model_image(image, *native, &error);
        if (!source) {
            out.warnings.push_back(image.name + ": " + error);
            continue;
        }
        TextureSettings texture_settings;
        texture_settings.kind = source->is_hdr() ? TextureKind::Hdr : image.kind;
        const std::string file = dir + "/image_" + std::to_string(i) + ".f3dtex";
        if (!fs.write(file, write_f3dtex(cook_texture(*source, texture_settings)))) {
            out.error = "cannot write " + file;
            return out;
        }
        out.outputs.push_back(file);
        report(progress, 0.7f + 0.25f * static_cast<float>(i + 1) / static_cast<float>(images));
    }
    if (!fs.write_text(dir + "/model.json", model.dump(2))) {
        out.error = "cannot write " + dir + "/model.json";
        return out;
    }
    out.outputs.push_back(dir + "/model.json");
    std::vector<std::string> dependencies;
    for (const std::string& file : loaded.scene->dependencies) {
        if (const auto path = core::vfs::parse(directory_of(asset.path) + file)) {
            dependencies.push_back(core::vfs::join(path->mount, path->relative));
        }
    }
    if (!record_import(fs, asset, settings.to_json(), hash, std::move(dependencies))) {
        out.warnings.push_back("cannot update " + asset.path + ".meta");
    }
    out.ok = true;
    return out;
}

ImportOutcome import_texture(core::vfs::FileSystem& fs, const AssetRecord& asset,
                             const core::jobs::CancellationToken* cancel, const ImportProgress& progress,
                             const core::vfs::Bytes& source, const std::string& hash) {
    ImportOutcome out;
    std::string error;
    const auto image = decode_texture_source(source, extension_of(asset.path), &error);
    if (!image) {
        out.error = asset.path + ": " + error;
        return out;
    }
    report(progress, 0.2f);
    const TextureSettings settings =
        TextureSettings::from_json(asset.settings, TextureSettings::defaults_for(asset.path));
    if (cancelled(cancel)) {
        out.cancelled = true;
        out.error = "cancelled";
        return out;
    }
    const CookedTexture cooked = cook_texture(*image, settings);
    report(progress, 0.9f);
    const std::string file = cooked_dir(asset.guid) + "/texture.f3dtex";
    if (!fs.write(file, write_f3dtex(cooked))) {
        out.error = "cannot write " + file;
        return out;
    }
    out.outputs.push_back(file);
    if (!record_import(fs, asset, settings.to_json(), hash)) {
        out.warnings.push_back("cannot update " + asset.path + ".meta");
    }
    out.ok = true;
    return out;
}

} // namespace

std::string cooked_dir(const core::Guid& guid, std::string_view mount) {
    return std::string(mount) + "://.forge3d/cache/" + guid.to_string();
}

ImportOutcome import_asset(core::vfs::FileSystem& fs, const AssetRecord& asset,
                           const core::jobs::CancellationToken* cancel, const ImportProgress& progress) {
    F3D_ZONE;
    ImportOutcome out;
    try {
        report(progress, 0.0f);
        const auto source = fs.read(asset.path);
        if (!source) {
            out.error = asset.path + ": cannot read";
            return out;
        }
        const std::string hash = hash_source(*source);
        if (asset.importer == "gltf" || asset.importer == "fbx" || asset.importer == "obj") {
            out = import_model(fs, asset, cancel, progress, hash);
        } else if (asset.importer == "texture" || asset.importer == "texture_hdr") {
            out = import_texture(fs, asset, cancel, progress, *source, hash);
        } else if (asset.importer == "material") {
            // Materials are used as they are: check it parses, record the hash.
            std::string error;
            if (!read_material(std::string_view(reinterpret_cast<const char*>(source->data()), source->size()),
                               &error)) {
                out.error = asset.path + ": " + error;
            } else {
                out.ok = record_import(fs, asset, nlohmann::json::object(), hash);
                if (!out.ok) {
                    out.error = "cannot update " + asset.path + ".meta";
                }
            }
        } else if (asset.importer == "prefab") {
            // Nothing to cook: prefabs are used as they are. Check it parses, record the hash.
            const nlohmann::json doc = nlohmann::json::parse(source->begin(), source->end(), nullptr, false);
            if (doc.is_discarded() || doc.value("format", std::string()) != "forge3d.prefab") {
                out.error = asset.path + ": not a forge3d prefab";
            } else {
                out.ok = record_import(fs, asset, nlohmann::json::object(), hash);
                if (!out.ok) {
                    out.error = "cannot update " + asset.path + ".meta";
                }
            }
        } else {
            out.error = asset.path + ": no importer '" + asset.importer + "'";
        }
        if (out.ok) {
            out.source_hash = hash;
            report(progress, 1.0f);
        }
    } catch (const std::exception& e) {
        out = {};
        out.error = asset.path + ": " + e.what();
    }
    return out;
}

std::optional<CookedModel> load_cooked_model(const core::vfs::FileSystem& fs, const core::Guid& guid,
                                             std::string* error) {
    F3D_ZONE;
    const auto fail = [&](std::string why) -> std::optional<CookedModel> {
        if (error) {
            *error = std::move(why);
        }
        return std::nullopt;
    };
    const std::string dir = cooked_dir(guid);
    const auto text = fs.read_text(dir + "/model.json");
    if (!text) {
        return fail("not cooked: " + dir + "/model.json");
    }
    try {
        const nlohmann::json model = nlohmann::json::parse(*text);
        if (model.value("format", std::string()) != kModelFormat) {
            return fail(dir + "/model.json: not a cooked model");
        }
        CookedModel out;
        out.name = model.value("name", std::string());
        for (const auto& node : model.at("nodes")) {
            out.nodes.push_back(node_from_json(node));
        }
        for (const auto& row : model.at("meshes")) {
            const std::string file = dir + "/" + row.at("file").get<std::string>();
            const auto bytes = fs.read(file);
            std::string mesh_error;
            std::optional<CookedMesh> mesh = bytes ? read_f3dmesh(*bytes, &mesh_error) : std::nullopt;
            if (!mesh) {
                return fail(file + ": " + (bytes ? mesh_error : std::string("missing")));
            }
            out.meshes.push_back(std::move(*mesh));
        }
        if (const auto it = model.find("materials"); it != model.end()) {
            for (const auto& row : *it) {
                std::string material_error;
                auto material = material_from_json(row, &material_error);
                if (!material) {
                    return fail(dir + "/model.json: material: " + material_error);
                }
                out.materials.push_back(std::move(*material));
            }
        }
        return out;
    } catch (const std::exception& e) {
        return fail(dir + "/model.json: " + e.what());
    }
}

namespace {

std::optional<CookedTexture> read_cooked(const core::vfs::FileSystem& fs, const std::string& file, std::string* error) {
    const auto bytes = fs.read(file);
    if (!bytes) {
        if (error) {
            *error = "not cooked: " + file;
        }
        return std::nullopt;
    }
    return read_f3dtex(*bytes, error);
}

} // namespace

std::optional<CookedTexture> load_cooked_texture(const core::vfs::FileSystem& fs, const core::Guid& guid,
                                                 std::string* error) {
    return read_cooked(fs, cooked_dir(guid) + "/texture.f3dtex", error);
}

std::optional<CookedTexture> load_cooked_image(const core::vfs::FileSystem& fs, const core::Guid& guid,
                                               std::size_t index, std::string* error) {
    return read_cooked(fs, cooked_dir(guid) + "/image_" + std::to_string(index) + ".f3dtex", error);
}

std::optional<CookedTexture> load_cooked_texture(const core::vfs::FileSystem& fs, const core::AssetRef& ref,
                                                 std::string* error) {
    const std::string_view sub = sub_asset(ref);
    if (sub.starts_with("image/")) {
        std::size_t index = 0;
        const auto [ptr, ec] = std::from_chars(sub.data() + 6, sub.data() + sub.size(), index);
        if (ec != std::errc() || ptr != sub.data() + sub.size()) {
            if (error) {
                *error = "bad image reference " + ref.path;
            }
            return std::nullopt;
        }
        return load_cooked_image(fs, ref.guid, index, error);
    }
    return load_cooked_texture(fs, ref.guid, error);
}

std::optional<SourceImage> decode_model_image(const ModelImage& image, const std::filesystem::path& model_file,
                                              std::string* error) {
    std::vector<std::uint8_t> bytes = image.bytes;
    if (bytes.empty() && !image.uri.empty()) {
        const std::filesystem::path file = model_file.parent_path() / std::filesystem::path(image.uri);
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            if (error) {
                *error = "cannot read " + file.string();
            }
            return std::nullopt;
        }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (bytes.empty()) {
        if (error) {
            *error = "no image data";
        }
        return std::nullopt;
    }
    return decode_texture_source(bytes, image.extension, error);
}

} // namespace f3d::assets
