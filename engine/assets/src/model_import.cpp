// SPDX-License-Identifier: Apache-2.0
// FBX (ufbx) and OBJ (tinyobjloader) importers into the common ModelScene
// intermediate, and the extension dispatcher (phases.md P3.2).
#include "f3d/assets/mesh.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/core/image.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <meshoptimizer.h>
#include <tiny_obj_loader.h>
#include <ufbx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>

namespace f3d::assets {
namespace {

std::string lower_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::uint32_t srgb_color(float r, float g, float b, float a = 1.0f) {
    return core::debug::rgba(core::linear_to_srgb(r), core::linear_to_srgb(g), core::linear_to_srgb(b), a);
}

/// Image slots referring to files next to the model, one ModelImage per distinct file.
class ImageTable {
public:
    explicit ImageTable(ModelScene& model) : model_(model) {}
    /// "#image/<n>" for `file` (relative to the model), or empty if `file` is empty.
    core::AssetRef ref(std::string file, TextureKind kind) {
        std::replace(file.begin(), file.end(), '\\', '/');
        while (!file.empty() && (file.back() == '\r' || file.back() == ' ')) {
            file.pop_back();
        }
        if (file.empty()) {
            return {};
        }
        auto [it, inserted] = index_.try_emplace(file, model_.images.size());
        if (inserted) {
            ModelImage image;
            image.name = std::filesystem::path(file).filename().string();
            image.uri = file;
            image.extension = lower_extension(file);
            image.kind = kind;
            model_.images.push_back(std::move(image));
        }
        return {core::Guid{}, "#image/" + std::to_string(it->second)};
    }

private:
    ModelScene& model_;
    std::map<std::string, std::size_t> index_;
};

/// Merges identical vertices (position, normal, colour, uv) of an unindexed or
/// loosely indexed mesh. Recomputes bounds.
void weld(MeshData& mesh) {
    if (mesh.vertices.empty()) {
        return;
    }
    std::vector<meshopt_Stream> streams{{mesh.vertices.data(), sizeof(MeshVertex), sizeof(MeshVertex)}};
    if (!mesh.uvs.empty()) {
        streams.push_back({mesh.uvs.data(), sizeof(glm::vec2), sizeof(glm::vec2)});
    }
    std::vector<unsigned int> remap(mesh.vertices.size());
    const std::size_t unique = meshopt_generateVertexRemapMulti(remap.data(), mesh.indices.data(), mesh.indices.size(),
                                                                mesh.vertices.size(), streams.data(), streams.size());
    meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), remap.data());
    std::vector<MeshVertex> vertices(unique);
    meshopt_remapVertexBuffer(vertices.data(), mesh.vertices.data(), mesh.vertices.size(), sizeof(MeshVertex),
                              remap.data());
    mesh.vertices = std::move(vertices);
    if (!mesh.uvs.empty()) {
        std::vector<glm::vec2> uvs(unique);
        meshopt_remapVertexBuffer(uvs.data(), mesh.uvs.data(), mesh.uvs.size(), sizeof(glm::vec2), remap.data());
        mesh.uvs = std::move(uvs);
    }
    mesh.bounds = {};
    for (const MeshVertex& v : mesh.vertices) {
        mesh.bounds.add(v.position);
    }
}

// --- FBX ----------------------------------------------------------------------------------------

glm::vec3 to_glm(const ufbx_vec3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

glm::mat4 to_glm(const ufbx_matrix& m) {
    glm::mat4 out(1.0f);
    for (int c = 0; c < 4; ++c) {
        out[c] = glm::vec4(to_glm(m.cols[c]), c == 3 ? 1.0f : 0.0f);
    }
    return out;
}

std::string fbx_texture_file(const ufbx_texture* texture) {
    if (!texture) {
        return {};
    }
    if (texture->relative_filename.length > 0) {
        return {texture->relative_filename.data, texture->relative_filename.length};
    }
    return {};
}

Material fbx_material(const ufbx_material& m, ImageTable& images) {
    Material out;
    out.name = m.name.length ? std::string(m.name.data, m.name.length) : "FBX material";
    if (m.pbr.base_color.has_value) {
        const ufbx_vec4& c = m.pbr.base_color.value_vec4;
        const float f = m.pbr.base_factor.has_value ? static_cast<float>(m.pbr.base_factor.value_real) : 1.0f;
        out.base_color = {static_cast<float>(c.x) * f, static_cast<float>(c.y) * f, static_cast<float>(c.z) * f, 1.0f};
    } else if (m.fbx.diffuse_color.has_value) {
        const ufbx_vec3& c = m.fbx.diffuse_color.value_vec3;
        out.base_color = {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 1.0f};
    }
    if (m.pbr.roughness.has_value) {
        out.roughness = std::clamp(static_cast<float>(m.pbr.roughness.value_real), 0.0f, 1.0f);
    }
    if (m.pbr.metalness.has_value) {
        out.metallic = std::clamp(static_cast<float>(m.pbr.metalness.value_real), 0.0f, 1.0f);
    }
    const ufbx_texture* base = m.pbr.base_color.texture ? m.pbr.base_color.texture : m.fbx.diffuse_color.texture;
    out.base_color_texture = images.ref(fbx_texture_file(base), kBaseColorKind);
    const ufbx_texture* normal = m.pbr.normal_map.texture ? m.pbr.normal_map.texture : m.fbx.normal_map.texture;
    out.normal_texture = images.ref(fbx_texture_file(normal), kNormalKind);
    return out;
}

/// The mesh of `node` with its geometry transform (pivots) baked in. Triangles are
/// grouped by material (indices into the scene's materials).
MeshData fbx_mesh(const ufbx_node& node, const ufbx_mesh& src) {
    MeshData mesh;
    mesh.name =
        src.name.length ? std::string(src.name.data, src.name.length) : std::string(node.name.data, node.name.length);
    const bool baked = node.has_geometry_transform;
    const glm::mat4 geometry = baked ? to_glm(node.geometry_to_node) : glm::mat4(1.0f);
    const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(geometry)));
    const bool has_uv = src.vertex_uv.exists;
    const bool has_normal = src.vertex_normal.exists;

    std::vector<std::uint32_t> triangle(src.max_face_triangles * 3);
    std::vector<std::uint32_t> triangle_material;
    for (std::size_t f = 0; f < src.faces.count; ++f) {
        const ufbx_face face = src.faces.data[f];
        const std::uint32_t triangles = ufbx_triangulate_face(triangle.data(), triangle.size(), &src, face);
        std::uint32_t material = SubMesh::kNoMaterial;
        if (src.face_material.count > f && src.materials.count > 0) {
            const std::uint32_t m = src.face_material.data[f];
            if (m < src.materials.count && src.materials.data[m]) {
                material = src.materials.data[m]->typed_id; // index into scene->materials
            }
        }
        triangle_material.insert(triangle_material.end(), triangles, material);
        const std::uint32_t color = core::debug::kWhite;
        for (std::uint32_t i = 0; i < triangles * 3; ++i) {
            const std::uint32_t index = triangle[i];
            MeshVertex v{};
            v.position =
                glm::vec3(geometry * glm::vec4(to_glm(ufbx_get_vertex_vec3(&src.vertex_position, index)), 1.0f));
            v.normal = has_normal
                           ? glm::normalize(normal_matrix * to_glm(ufbx_get_vertex_vec3(&src.vertex_normal, index)))
                           : glm::vec3(0.0f, 1.0f, 0.0f);
            v.color = color;
            mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
            mesh.vertices.push_back(v);
            if (has_uv) {
                const ufbx_vec2 uv = ufbx_get_vertex_vec2(&src.vertex_uv, index);
                // FBX UVs have V up; glTF's (and ours) have V down.
                mesh.uvs.emplace_back(static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y));
            }
        }
    }
    if (!has_normal) {
        compute_normals(mesh);
    }
    mesh.primitive_count = mesh.indices.empty() ? 0 : 1;
    weld(mesh);
    mesh.submeshes = group_by_material(mesh, triangle_material);
    return mesh;
}

} // namespace

ModelLoadResult load_fbx_scene(const std::filesystem::path& path) {
    F3D_ZONE;
    ModelLoadResult result;
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    ufbx_error error{};
    const std::u8string utf8 = path.u8string();
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_file_len(reinterpret_cast<const char*>(utf8.data()), utf8.size(), &opts, &error), &ufbx_free_scene);
    if (!scene) {
        char text[512];
        ufbx_format_error(text, sizeof(text), &error);
        result.error = path.filename().string() + ": " + text;
        return result;
    }

    ModelScene model;
    model.name = path.stem().string();
    ImageTable images(model);
    for (std::size_t i = 0; i < scene->materials.count; ++i) {
        model.materials.push_back(fbx_material(*scene->materials.data[i], images));
    }
    for (std::size_t i = 0; i < scene->textures.count; ++i) {
        const ufbx_texture* texture = scene->textures.data[i];
        if (texture->relative_filename.length > 0) {
            std::string file(texture->relative_filename.data, texture->relative_filename.length);
            std::replace(file.begin(), file.end(), '\\', '/');
            model.dependencies.push_back(std::move(file));
        }
    }
    struct Item {
        const ufbx_node* node;
        int parent;
    };
    std::vector<Item> stack;
    const ufbx_node* root = scene->root_node;
    for (std::size_t i = root->children.count; i-- > 0;) {
        stack.push_back({root->children.data[i], -1});
    }
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const ufbx_node& node = *item.node;
        ModelNode out;
        out.name = node.name.length ? std::string(node.name.data, node.name.length) : "Node";
        out.parent = item.parent;
        out.translation = to_glm(node.local_transform.translation);
        const ufbx_quat& q = node.local_transform.rotation;
        out.rotation = glm::quat(static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y),
                                 static_cast<float>(q.z));
        out.scale = to_glm(node.local_transform.scale);
        if (node.mesh) {
            MeshData mesh = fbx_mesh(node, *node.mesh);
            if (!mesh.indices.empty()) {
                out.mesh = static_cast<int>(model.meshes.size());
                model.meshes.push_back(std::move(mesh));
            }
        }
        const int index = static_cast<int>(model.nodes.size());
        model.nodes.push_back(std::move(out));
        for (std::size_t i = node.children.count; i-- > 0;) {
            stack.push_back({node.children.data[i], index});
        }
    }
    if (model.meshes.empty()) {
        result.error = path.filename().string() + ": no meshes";
        return result;
    }
    F3D_LOG_INFO("Loaded {}: {} nodes, {} meshes", path.filename().string(), model.nodes.size(), model.meshes.size());
    result.scene = std::move(model);
    return result;
}

// --- OBJ ----------------------------------------------------------------------------------------

ModelLoadResult load_obj_scene(const std::filesystem::path& path) {
    F3D_ZONE;
    ModelLoadResult result;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = true;
    config.mtl_search_path = path.parent_path().string();
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path.string(), config)) {
        result.error = path.filename().string() + ": " + (reader.Error().empty() ? "cannot read" : reader.Error());
        return result;
    }
    ModelScene model;
    model.name = path.stem().string();
    if (!reader.Warning().empty()) {
        model.warnings.push_back(reader.Warning());
    }
    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    const auto& materials = reader.GetMaterials();
    ImageTable images(model);
    for (const tinyobj::material_t& m : materials) {
        Material out;
        out.name = m.name.empty() ? model.name + " material" : m.name;
        out.base_color = {m.diffuse[0], m.diffuse[1], m.diffuse[2], 1.0f};
        // PBR extension values when given, else roughness from the Phong exponent.
        out.roughness = m.roughness > 0.0f ? m.roughness : std::sqrt(2.0f / (std::max(m.shininess, 0.0f) + 2.0f));
        out.metallic = m.metallic;
        out.emissive = {m.emission[0], m.emission[1], m.emission[2]};
        out.base_color_texture = images.ref(m.diffuse_texname, kBaseColorKind);
        out.normal_texture = images.ref(m.normal_texname, kNormalKind);
        model.materials.push_back(std::move(out));
    }
    {
        // Dependencies: the material libraries and every texture they name.
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("mtllib ", 0) == 0) {
                model.dependencies.push_back(line.substr(7));
            }
        }
        for (const tinyobj::material_t& m : materials) {
            for (const std::string* file :
                 {&m.diffuse_texname, &m.specular_texname, &m.bump_texname, &m.normal_texname, &m.alpha_texname,
                  &m.roughness_texname, &m.metallic_texname, &m.emissive_texname, &m.ambient_texname}) {
                if (!file->empty()) {
                    model.dependencies.push_back(*file);
                }
            }
        }
        for (std::string& file : model.dependencies) {
            std::replace(file.begin(), file.end(), '\\', '/');
            while (!file.empty() && (file.back() == '\r' || file.back() == ' ')) {
                file.pop_back();
            }
        }
    }
    for (const tinyobj::shape_t& shape : reader.GetShapes()) {
        if (shape.mesh.indices.empty()) {
            continue;
        }
        MeshData mesh;
        mesh.name = shape.name.empty() ? model.name : shape.name;
        bool has_normals = true;
        bool has_uvs = true;
        for (const tinyobj::index_t& index : shape.mesh.indices) {
            has_normals = has_normals && index.normal_index >= 0;
            has_uvs = has_uvs && index.texcoord_index >= 0;
        }
        // One vertex per distinct (position, normal, uv, material) tuple, so a mesh
        // without normals is connected before they are computed (smooth, not faceted).
        std::map<std::array<int, 4>, std::uint32_t> unique;
        std::vector<std::uint32_t> triangle_material;
        for (std::size_t i = 0; i < shape.mesh.indices.size(); ++i) {
            const tinyobj::index_t& index = shape.mesh.indices[i];
            const std::size_t face = i / 3;
            const int material_id = face < shape.mesh.material_ids.size() ? shape.mesh.material_ids[face] : -1;
            if (i % 3 == 0) {
                triangle_material.push_back(material_id >= 0 && static_cast<std::size_t>(material_id) < materials.size()
                                                ? static_cast<std::uint32_t>(material_id)
                                                : SubMesh::kNoMaterial);
            }
            const std::array<int, 4> key{index.vertex_index, has_normals ? index.normal_index : -1,
                                         has_uvs ? index.texcoord_index : -1, material_id};
            if (const auto it = unique.find(key); it != unique.end()) {
                mesh.indices.push_back(it->second);
                continue;
            }
            unique.emplace(key, static_cast<std::uint32_t>(mesh.vertices.size()));
            MeshVertex v{};
            const auto p = static_cast<std::size_t>(index.vertex_index) * 3;
            v.position = {attrib.vertices[p], attrib.vertices[p + 1], attrib.vertices[p + 2]};
            if (has_normals) {
                const auto n = static_cast<std::size_t>(index.normal_index) * 3;
                v.normal = glm::normalize(glm::vec3(attrib.normals[n], attrib.normals[n + 1], attrib.normals[n + 2]));
            }
            // Material colours live in the materials now; only real vertex colours go here.
            if (!attrib.colors.empty() &&
                (material_id < 0 || static_cast<std::size_t>(material_id) >= materials.size())) {
                v.color = srgb_color(attrib.colors[p], attrib.colors[p + 1], attrib.colors[p + 2]);
            } else {
                v.color = core::debug::kWhite;
            }
            mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
            mesh.vertices.push_back(v);
            if (has_uvs) {
                const auto t = static_cast<std::size_t>(index.texcoord_index) * 2;
                mesh.uvs.emplace_back(attrib.texcoords[t], 1.0f - attrib.texcoords[t + 1]); // OBJ V is up
            }
        }
        if (!has_normals) {
            compute_normals(mesh);
        }
        mesh.primitive_count = 1;
        weld(mesh); // equal tuples with equal values (e.g. duplicated normals) merge too
        mesh.submeshes = group_by_material(mesh, triangle_material);
        ModelNode node;
        node.name = mesh.name;
        node.mesh = static_cast<int>(model.meshes.size());
        model.meshes.push_back(std::move(mesh));
        model.nodes.push_back(std::move(node));
    }
    if (model.meshes.empty()) {
        result.error = path.filename().string() + ": no faces";
        return result;
    }
    F3D_LOG_INFO("Loaded {}: {} shapes", path.filename().string(), model.meshes.size());
    result.scene = std::move(model);
    return result;
}

// --- Dispatch -----------------------------------------------------------------------------------

bool is_model_file(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    return ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj";
}

ModelLoadResult load_model(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    if (ext == ".gltf" || ext == ".glb") {
        return load_gltf_scene(path);
    }
    if (ext == ".fbx") {
        return load_fbx_scene(path);
    }
    if (ext == ".obj") {
        return load_obj_scene(path);
    }
    ModelLoadResult result;
    result.error = path.filename().string() + ": unsupported model format";
    return result;
}

} // namespace f3d::assets
