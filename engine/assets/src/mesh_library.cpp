// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/mesh_library.hpp"

#include "f3d/assets/pipeline.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>

namespace f3d::assets {
namespace {

struct SubAsset {
    std::string file;
    std::size_t mesh = 0;
    bool valid = false;
};

/// Splits "<file>#<kind>/<n>" ("#mesh/", "#material/", "#image/").
SubAsset parse_sub_asset(const std::string& path, std::string_view kind) {
    SubAsset out;
    const std::string marker = "#" + std::string(kind) + "/";
    const auto hash = path.rfind(marker);
    if (hash == std::string::npos) {
        return out;
    }
    out.file = path.substr(0, hash);
    const char* begin = path.data() + hash + marker.size();
    const char* end = path.data() + path.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out.mesh);
    out.valid = ec == std::errc() && ptr == end && begin != end;
    return out;
}

SubAsset parse_mesh_path(const std::string& path) {
    return parse_sub_asset(path, "mesh");
}

/// Cache key of a GUID reference: the GUID plus the sub-asset suffix (paths may move).
std::string guid_key(const core::AssetRef& ref) {
    const auto hash = ref.path.rfind('#');
    return ref.guid.to_string() + (hash == std::string::npos ? "" : ref.path.substr(hash));
}

} // namespace

std::shared_ptr<const GltfScene> MeshLibrary::load_file(const std::filesystem::path& file, std::string* error) {
    const std::string key = file.generic_string();
    if (const auto it = files_.find(key); it != files_.end()) {
        return it->second;
    }
    auto loaded = load_model(file); // glTF, FBX or OBJ
    if (!loaded.scene) {
        if (error) {
            *error = loaded.error;
        }
        return nullptr;
    }
    auto scene = std::make_shared<const GltfScene>(std::move(*loaded.scene));
    for (std::size_t i = 0; i < scene->meshes.size(); ++i) {
        // Aliasing pointers: the meshes live inside the cached scene.
        meshes_[gltf_mesh_path(file, i)] = std::shared_ptr<const MeshData>(scene, &scene->meshes[i]);
    }
    files_.emplace(key, scene);
    ++generation_;
    return scene;
}

std::shared_ptr<const MeshData> MeshLibrary::get(const core::AssetRef& ref) {
    if (!ref.guid.is_null() && resolver_) {
        // A project asset: the cooked mesh, keyed by GUID + sub-asset (paths may move).
        const auto hash = ref.path.rfind('#');
        const std::string key = ref.guid.to_string() + (hash == std::string::npos ? "" : ref.path.substr(hash));
        if (const auto it = meshes_.find(key); it != meshes_.end()) {
            return it->second;
        }
        if (failed_.contains(key)) {
            return nullptr;
        }
        auto mesh = resolver_(ref);
        if (!mesh) {
            F3D_LOG_ERROR("Mesh {} ({}): not available (not imported yet?)", ref.path, ref.guid.to_string());
            failed_.insert(key);
            return nullptr;
        }
        meshes_[key] = mesh;
        ++generation_;
        return mesh;
    }
    if (const auto it = meshes_.find(ref.path); it != meshes_.end()) {
        return it->second;
    }
    if (ref.path.empty() || failed_.contains(ref.path)) {
        return nullptr;
    }
    const SubAsset sub = parse_mesh_path(ref.path);
    std::string error = "not a mesh asset path (expected <file>#mesh/<n>)";
    if (sub.valid && load_file(std::filesystem::path(sub.file), &error)) {
        if (const auto it = meshes_.find(ref.path); it != meshes_.end()) {
            return it->second;
        }
        error = "the file has no mesh " + std::to_string(sub.mesh);
    }
    F3D_LOG_ERROR("Mesh {}: {}", ref.path, error);
    failed_.insert(ref.path);
    return nullptr;
}

std::shared_ptr<const Material> MeshLibrary::material(const core::AssetRef& ref) {
    if (ref.empty()) {
        return nullptr;
    }
    const std::string key = ref.guid.is_null() ? ref.path : guid_key(ref);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    if (failed_.contains("material:" + key)) {
        return nullptr;
    }
    std::shared_ptr<const Material> result;
    std::string error = "not available";
    const bool vfs_path = ref.path.find("://") != std::string::npos;
    if ((!ref.guid.is_null() || vfs_path) && material_resolver_) {
        result = material_resolver_(ref);
    } else if (ref.guid.is_null()) {
        if (const SubAsset sub = parse_sub_asset(ref.path, "material"); sub.valid) {
            if (const auto scene = load_file(std::filesystem::path(sub.file), &error)) {
                if (sub.mesh < scene->materials.size()) {
                    result = std::make_shared<const Material>(
                        rebase_textures(scene->materials[sub.mesh], core::AssetRef{{}, sub.file}));
                } else {
                    error = "the file has no material " + std::to_string(sub.mesh);
                }
            }
        } else if (ref.path.ends_with(kMaterialExtension)) {
            std::ifstream in{std::filesystem::path(ref.path)};
            std::stringstream text;
            text << in.rdbuf();
            if (auto m = in ? read_material(text.str(), &error) : std::nullopt) {
                result = std::make_shared<const Material>(std::move(*m));
            } else if (!in) {
                error = "cannot read the file";
            }
        } else {
            error = "not a material reference";
        }
    }
    if (!result) {
        F3D_LOG_ERROR("Material {}: {}", ref.path, error);
        failed_.insert("material:" + key);
        return nullptr;
    }
    materials_[key] = result;
    ++generation_;
    return result;
}

core::AssetRef MeshLibrary::default_material(const core::AssetRef& mesh, std::uint32_t material_index) {
    if (material_index == SubMesh::kNoMaterial || mesh.path.empty()) {
        return {};
    }
    return {mesh.guid, std::string(asset_file(mesh)) + "#material/" + std::to_string(material_index)};
}

core::AssetRef MeshLibrary::slot_material(const scene::MeshRenderer& renderer, std::size_t slot,
                                          const SubMesh& submesh) {
    if (slot < renderer.materials.size() && !renderer.materials[slot].empty()) {
        return renderer.materials[slot];
    }
    return default_material(renderer.mesh, submesh.material);
}

std::optional<SourceImage> MeshLibrary::image(const core::AssetRef& ref, TextureKind* kind) {
    const SubAsset sub = parse_sub_asset(ref.path, "image");
    std::string error = "not an image";
    if (!sub.valid) {
        if (image_resolver_) {
            if (auto resolved = image_resolver_(ref)) {
                return resolved;
            }
        }
        if (!ref.path.empty() && ref.path.find("://") == std::string::npos) {
            std::ifstream in(std::filesystem::path(ref.path), std::ios::binary);
            if (in) {
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                                      std::istreambuf_iterator<char>());
                std::string extension = std::filesystem::path(ref.path).extension().string();
                for (char& c : extension) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (auto decoded = decode_texture_source(bytes, extension, &error)) {
                    return decoded;
                }
            } else {
                error = "cannot open the file";
            }
        }
    } else {
        if (const auto scene = load_file(std::filesystem::path(sub.file), &error)) {
            if (sub.mesh < scene->images.size()) {
                const ModelImage& image = scene->images[sub.mesh];
                if (kind) {
                    *kind = image.kind;
                }
                if (auto decoded = decode_model_image(image, std::filesystem::path(sub.file), &error)) {
                    return decoded;
                }
            } else {
                error = "the file has no image " + std::to_string(sub.mesh);
            }
        }
    }
    if (failed_.insert("image:" + ref.path).second) {
        F3D_LOG_ERROR("Image {}: {}", ref.path, error);
    }
    return std::nullopt;
}

void MeshLibrary::add_material(const std::string& path, Material material) {
    materials_[path] = std::make_shared<const Material>(std::move(material));
    failed_.erase("material:" + path);
    ++generation_;
}

void MeshLibrary::invalidate_materials() {
    materials_.clear();
    std::erase_if(failed_, [](const std::string& key) { return key.starts_with("material:"); });
    ++generation_;
}

void MeshLibrary::add(const std::string& path, MeshData mesh) {
    meshes_[path] = std::make_shared<const MeshData>(std::move(mesh));
    failed_.erase(path);
    ++generation_;
}

void MeshLibrary::invalidate(const core::Guid& guid) {
    const std::string prefix = guid.to_string();
    std::erase_if(meshes_, [&](const auto& entry) { return entry.first.starts_with(prefix); });
    std::erase_if(materials_, [&](const auto& entry) { return entry.first.starts_with(prefix); });
    std::erase_if(failed_, [&](const std::string& key) {
        return key.starts_with(prefix) || key.starts_with("material:" + prefix);
    });
    ++generation_;
}

void MeshLibrary::clear() {
    meshes_.clear();
    files_.clear();
    materials_.clear();
    failed_.clear();
    ++generation_;
}

nlohmann::json gltf_import_ops(const GltfScene& gltf, const std::filesystem::path& file, const core::Guid& parent,
                               core::Guid& root_guid) {
    return model_instance_ops(
        gltf.name, gltf.nodes, [&](std::size_t mesh) { return core::AssetRef{{}, gltf_mesh_path(file, mesh)}; }, parent,
        glm::vec3(0.0f), root_guid);
}

nlohmann::json model_instance_ops(const std::string& name, const std::vector<ModelNode>& nodes,
                                  const std::function<core::AssetRef(std::size_t)>& mesh_ref, const core::Guid& parent,
                                  const glm::vec3& root_translation, core::Guid& root_guid) {
    using nlohmann::json;
    const auto guid_json = [](const core::Guid& g) { return g.is_null() ? json() : json(g.to_string()); };
    json ops = json::array();
    root_guid = core::Guid::generate();
    ops.push_back({{"op", "entity.create"},
                   {"params", {{"name", name}, {"guid", root_guid.to_string()}, {"parent", guid_json(parent)}}}});
    if (root_translation != glm::vec3(0.0f)) {
        ops.push_back({{"op", "entity.set_property"},
                       {"params",
                        {{"entity", root_guid.to_string()},
                         {"path", "Transform.translation"},
                         {"value", {root_translation.x, root_translation.y, root_translation.z}}}}});
    }
    std::vector<core::Guid> guids(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const ModelNode& node = nodes[i];
        guids[i] = core::Guid::generate();
        const core::Guid& node_parent = node.parent < 0 ? root_guid : guids[static_cast<std::size_t>(node.parent)];
        const std::string guid = guids[i].to_string();
        ops.push_back({{"op", "entity.create"},
                       {"params", {{"name", node.name}, {"guid", guid}, {"parent", node_parent.to_string()}}}});
        const auto set = [&](const char* path, json value) {
            ops.push_back({{"op", "entity.set_property"},
                           {"params", {{"entity", guid}, {"path", path}, {"value", std::move(value)}}}});
        };
        if (node.translation != glm::vec3(0.0f)) {
            set("Transform.translation", {node.translation.x, node.translation.y, node.translation.z});
        }
        if (node.rotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
            set("Transform.rotation", {node.rotation.w, node.rotation.x, node.rotation.y, node.rotation.z});
        }
        if (node.scale != glm::vec3(1.0f)) {
            set("Transform.scale", {node.scale.x, node.scale.y, node.scale.z});
        }
        if (node.mesh >= 0) {
            const core::AssetRef ref = mesh_ref(static_cast<std::size_t>(node.mesh));
            const json mesh = {{"guid", guid_json(ref.guid)}, {"path", ref.path}};
            ops.push_back({{"op", "entity.add_component"},
                           {"params", {{"entity", guid}, {"component", "MeshRenderer"}, {"value", {{"mesh", mesh}}}}}});
        }
    }
    return ops;
}

entt::entity spawn_gltf(scene::World& world, const GltfScene& gltf, const std::filesystem::path& file,
                        entt::entity parent) {
    F3D_ZONE;
    const entt::entity root = world.create(gltf.name, parent);
    std::vector<entt::entity> created(gltf.nodes.size(), static_cast<entt::entity>(entt::null));
    for (std::size_t i = 0; i < gltf.nodes.size(); ++i) {
        const GltfNode& node = gltf.nodes[i];
        const entt::entity p = node.parent < 0 ? root : created[static_cast<std::size_t>(node.parent)];
        const entt::entity e = world.create(node.name, p);
        world.set_local(e, node.translation, node.rotation, node.scale);
        if (node.mesh >= 0) {
            world.registry().emplace<scene::MeshRenderer>(
                e, scene::MeshRenderer{{{}, gltf_mesh_path(file, static_cast<std::size_t>(node.mesh))}, true});
        }
        created[i] = e;
    }
    return root;
}

core::Bounds world_bounds(scene::World& world, MeshLibrary& meshes, entt::entity root) {
    world.update_transforms();
    core::Bounds bounds;
    const auto add = [&](entt::entity e) {
        const auto* renderer = world.registry().try_get<scene::MeshRenderer>(e);
        if (!renderer) {
            return;
        }
        if (const auto mesh = meshes.get(renderer->mesh)) {
            bounds.add(mesh->bounds.transformed(world.transform(e).world));
        }
    };
    if (root == entt::null) {
        for (const auto e : world.registry().view<scene::MeshRenderer>()) {
            add(e);
        }
    } else {
        for (const auto e : world.subtree(root)) {
            add(e);
        }
    }
    return bounds;
}

} // namespace f3d::assets
