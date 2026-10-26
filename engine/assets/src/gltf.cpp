// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/mesh.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/core/image.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>
#include <variant>

namespace f3d::assets {
namespace {

void read_materials(const fastgltf::Asset& gltf, ModelScene& scene);

glm::mat4 to_glm(const fastgltf::math::fmat4x4& m) {
    glm::mat4 out;
    std::memcpy(&out, m.data(), sizeof(out)); // both column-major
    return out;
}

std::uint32_t srgb_color(const fastgltf::math::nvec4& linear) {
    return core::debug::rgba(core::linear_to_srgb(static_cast<float>(linear.x())),
                             core::linear_to_srgb(static_cast<float>(linear.y())),
                             core::linear_to_srgb(static_cast<float>(linear.z())), static_cast<float>(linear.w()));
}

} // namespace

void compute_normals(MeshData& mesh, std::size_t first_vertex, std::size_t first_index) {
    for (std::size_t v = first_vertex; v < mesh.vertices.size(); ++v) {
        mesh.vertices[v].normal = glm::vec3(0.0f);
    }
    for (std::size_t i = first_index; i + 2 < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i + 1]];
        auto& c = mesh.vertices[mesh.indices[i + 2]];
        const glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position); // |n| = 2 * area
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (std::size_t v = first_vertex; v < mesh.vertices.size(); ++v) {
        auto& n = mesh.vertices[v].normal;
        const float len = glm::length(n);
        n = len > 1e-12f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

namespace {

std::optional<fastgltf::Asset> parse(const std::filesystem::path& path, fastgltf::Options extra, std::string& error) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        error = "cannot read " + path.string() + ": " + std::string(fastgltf::getErrorMessage(data.error()));
        return std::nullopt;
    }
    fastgltf::Parser parser(fastgltf::Extensions::KHR_materials_emissive_strength);
    auto asset =
        parser.loadGltf(data.get(), path.parent_path(),
                        fastgltf::Options::LoadExternalBuffers | fastgltf::Options::GenerateMeshIndices | extra);
    if (asset.error() != fastgltf::Error::None) {
        error = path.filename().string() + ": " + std::string(fastgltf::getErrorMessage(asset.error()));
        return std::nullopt;
    }
    return std::move(asset.get());
}

/// Appends every triangle primitive of glTF mesh `mesh_index` to `mesh`, transformed by `matrix`.
/// `bake_colors`: the material's base colour goes into the vertex colours (the flattened
/// P1.4 path); otherwise vertices are white and each primitive becomes a submesh with its
/// material. Returns the number of skipped (non-triangle) primitives.
std::size_t append_mesh(const fastgltf::Asset& gltf, std::size_t mesh_index, const glm::mat4& matrix, MeshData& mesh,
                        bool bake_colors) {
    std::size_t skipped = 0;
    const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(matrix)));
    const bool mirrored = glm::determinant(glm::mat3(matrix)) < 0.0f;
    for (const fastgltf::Primitive& prim : gltf.meshes[mesh_index].primitives) {
        const auto position_it = prim.findAttribute("POSITION");
        if (prim.type != fastgltf::PrimitiveType::Triangles || position_it == prim.attributes.end() ||
            !prim.indicesAccessor) {
            ++skipped;
            continue;
        }
        const std::size_t first_vertex = mesh.vertices.size();
        const std::size_t first_index = mesh.indices.size();
        std::uint32_t color = core::debug::kWhite;
        if (prim.materialIndex && bake_colors) {
            color = srgb_color(gltf.materials[*prim.materialIndex].pbrData.baseColorFactor);
        }

        const fastgltf::Accessor& positions = gltf.accessors[position_it->accessorIndex];
        mesh.vertices.resize(first_vertex + positions.count);
        fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, positions, [&](glm::vec3 p, std::size_t i) {
            auto& v = mesh.vertices[first_vertex + i];
            v.position = glm::vec3(matrix * glm::vec4(p, 1.0f));
            v.color = color;
            mesh.bounds.add(v.position);
        });

        const fastgltf::Accessor& indices = gltf.accessors[*prim.indicesAccessor];
        mesh.indices.resize(first_index + indices.count);
        fastgltf::copyFromAccessor<std::uint32_t>(gltf, indices, mesh.indices.data() + first_index);
        for (std::size_t i = first_index; i < mesh.indices.size(); ++i) {
            mesh.indices[i] += static_cast<std::uint32_t>(first_vertex);
        }
        // A mirroring transform flips the winding; keep triangles front-facing.
        if (mirrored) {
            for (std::size_t i = first_index; i + 2 < mesh.indices.size(); i += 3) {
                std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
            }
        }

        // UVs: kept only if every primitive so far had them (a mesh's streams are all-or-nothing).
        const auto uv_it = prim.findAttribute("TEXCOORD_0");
        if (uv_it != prim.attributes.end() && mesh.uvs.size() == first_vertex) {
            mesh.uvs.resize(mesh.vertices.size(), glm::vec2(0.0f));
            fastgltf::iterateAccessorWithIndex<glm::vec2>(
                gltf, gltf.accessors[uv_it->accessorIndex],
                [&](glm::vec2 uv, std::size_t i) { mesh.uvs[first_vertex + i] = uv; });
        } else if (!mesh.uvs.empty()) {
            mesh.uvs.clear();
        }

        const auto normal_it = prim.findAttribute("NORMAL");
        if (normal_it != prim.attributes.end()) {
            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                gltf, gltf.accessors[normal_it->accessorIndex], [&](glm::vec3 n, std::size_t i) {
                    mesh.vertices[first_vertex + i].normal = glm::normalize(normal_matrix * n);
                });
        } else {
            compute_normals(mesh, first_vertex, first_index);
        }
        if (!bake_colors) {
            const std::uint32_t material =
                prim.materialIndex ? static_cast<std::uint32_t>(*prim.materialIndex) : SubMesh::kNoMaterial;
            const auto count = static_cast<std::uint32_t>(mesh.indices.size() - first_index);
            if (!mesh.submeshes.empty() && mesh.submeshes.back().material == material) {
                mesh.submeshes.back().index_count += count; // primitives with one material merge
            } else {
                mesh.submeshes.push_back({static_cast<std::uint32_t>(first_index), count, material});
            }
        }
        ++mesh.primitive_count;
    }
    return skipped;
}

} // namespace

MeshLoadResult load_gltf(const std::filesystem::path& path) {
    F3D_ZONE;
    MeshLoadResult result;
    auto gltf = parse(path, fastgltf::Options::None, result.error);
    if (!gltf) {
        return result;
    }

    MeshData mesh;
    mesh.name = path.stem().string();
    std::size_t skipped = 0;

    const auto add_node = [&](const fastgltf::Node& node, const fastgltf::math::fmat4x4& world_matrix) {
        if (node.meshIndex) {
            skipped += append_mesh(*gltf, *node.meshIndex, to_glm(world_matrix), mesh, true);
        }
    };
    if (!gltf->scenes.empty()) {
        fastgltf::iterateSceneNodes(*gltf, gltf->defaultScene.value_or(0), fastgltf::math::fmat4x4(), add_node);
    } else {
        // No scene: take every mesh node at its own transform.
        for (const auto& node : gltf->nodes) {
            add_node(node, fastgltf::getTransformMatrix(node));
        }
    }
    if (mesh.primitive_count == 0) {
        result.error = path.filename().string() + ": no triangle meshes";
        return result;
    }
    if (skipped > 0) {
        F3D_LOG_WARN("{}: skipped {} non-triangle primitives", path.filename().string(), skipped);
    }
    F3D_LOG_INFO("Loaded {}: {} primitives, {} vertices, {} triangles", path.filename().string(), mesh.primitive_count,
                 mesh.vertices.size(), mesh.indices.size() / 3);
    result.mesh = std::move(mesh);
    return result;
}

namespace {

std::string extension_for(fastgltf::MimeType mime, std::string_view uri) {
    switch (mime) {
    case fastgltf::MimeType::PNG:
        return ".png";
    case fastgltf::MimeType::JPEG:
        return ".jpg";
    default:
        break;
    }
    std::string ext = std::filesystem::path(uri).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

void copy_bytes(std::span<const std::byte> bytes, std::vector<std::uint8_t>& out) {
    out.resize(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
}

/// The bytes of a buffer (loaded by the parser), or an empty span.
std::span<const std::byte> buffer_bytes(const fastgltf::Buffer& buffer) {
    return std::visit(fastgltf::visitor{
                          [](const fastgltf::sources::Array& a) {
                              return std::span<const std::byte>(a.bytes.data(), a.bytes.size());
                          },
                          [](const fastgltf::sources::Vector& v) {
                              return std::span<const std::byte>(v.bytes.data(), v.bytes.size());
                          },
                          [](const fastgltf::sources::ByteView& v) {
                              return std::span<const std::byte>(v.bytes.data(), v.bytes.size());
                          },
                          [](const auto&) { return std::span<const std::byte>(); },
                      },
                      buffer.data);
}

ModelImage read_image(const fastgltf::Asset& gltf, const fastgltf::Image& image, std::size_t index) {
    ModelImage out;
    out.name = image.name.empty() ? "Image " + std::to_string(index) : std::string(image.name);
    std::visit(fastgltf::visitor{
                   [&](const fastgltf::sources::URI& uri) {
                       if (uri.uri.isLocalPath()) {
                           out.uri = uri.uri.fspath().generic_string();
                       }
                       out.extension = extension_for(uri.mimeType, uri.uri.path());
                   },
                   [&](const fastgltf::sources::BufferView& view) {
                       const fastgltf::BufferView& bv = gltf.bufferViews[view.bufferViewIndex];
                       const auto bytes = buffer_bytes(gltf.buffers[bv.bufferIndex]);
                       if (bv.byteOffset + bv.byteLength <= bytes.size()) {
                           copy_bytes(bytes.subspan(bv.byteOffset, bv.byteLength), out.bytes);
                       }
                       out.extension = extension_for(view.mimeType, {});
                   },
                   [&](const fastgltf::sources::Array& a) {
                       copy_bytes({a.bytes.data(), a.bytes.size()}, out.bytes);
                       out.extension = extension_for(a.mimeType, {});
                   },
                   [&](const fastgltf::sources::Vector& v) {
                       copy_bytes({v.bytes.data(), v.bytes.size()}, out.bytes);
                       out.extension = extension_for(v.mimeType, {});
                   },
                   [](const auto&) {},
               },
               image.data);
    return out;
}

void read_materials(const fastgltf::Asset& gltf, ModelScene& scene) {
    for (std::size_t i = 0; i < gltf.images.size(); ++i) {
        scene.images.push_back(read_image(gltf, gltf.images[i], i));
    }
    std::vector<bool> kind_set(scene.images.size(), false);
    // A texture slot: "#image/<n>"; the image's kind comes from its first use.
    const auto texture = [&](std::size_t texture_index, TextureKind kind) -> core::AssetRef {
        if (texture_index >= gltf.textures.size() || !gltf.textures[texture_index].imageIndex) {
            return {};
        }
        const std::size_t image = *gltf.textures[texture_index].imageIndex;
        if (image >= scene.images.size()) {
            return {};
        }
        if (!kind_set[image]) {
            scene.images[image].kind = kind;
            kind_set[image] = true;
        }
        return {core::Guid{}, "#image/" + std::to_string(image)};
    };
    for (std::size_t i = 0; i < gltf.materials.size(); ++i) {
        const fastgltf::Material& m = gltf.materials[i];
        Material out;
        out.name = m.name.empty() ? scene.name + " material " + std::to_string(i) : std::string(m.name);
        const auto& c = m.pbrData.baseColorFactor;
        out.base_color = {c.x(), c.y(), c.z(), c.w()};
        out.metallic = static_cast<float>(m.pbrData.metallicFactor);
        out.roughness = static_cast<float>(m.pbrData.roughnessFactor);
        out.emissive = {m.emissiveFactor.x(), m.emissiveFactor.y(), m.emissiveFactor.z()};
        out.emissive_strength = static_cast<float>(m.emissiveStrength);
        out.alpha_mode = m.alphaMode == fastgltf::AlphaMode::Mask ? AlphaMode::Mask : AlphaMode::Opaque;
        out.alpha_cutoff = static_cast<float>(m.alphaCutoff);
        out.double_sided = m.doubleSided;
        if (m.pbrData.baseColorTexture) {
            out.base_color_texture = texture(m.pbrData.baseColorTexture->textureIndex, kBaseColorKind);
        }
        if (m.normalTexture) {
            out.normal_texture = texture(m.normalTexture->textureIndex, kNormalKind);
            out.normal_scale = static_cast<float>(m.normalTexture->scale);
        }
        if (m.pbrData.metallicRoughnessTexture) {
            out.orm_texture = texture(m.pbrData.metallicRoughnessTexture->textureIndex, kOrmKind);
        }
        if (m.occlusionTexture) {
            out.occlusion_strength = static_cast<float>(m.occlusionTexture->strength);
            // glTF often packs occlusion into the metal/rough texture's red channel.
            if (m.pbrData.metallicRoughnessTexture &&
                m.occlusionTexture->textureIndex == m.pbrData.metallicRoughnessTexture->textureIndex) {
                out.orm_occlusion = true;
            } else {
                out.occlusion_texture = texture(m.occlusionTexture->textureIndex, kOcclusionKind);
            }
        }
        if (m.emissiveTexture) {
            out.emissive_texture = texture(m.emissiveTexture->textureIndex, kEmissiveKind);
        }
        scene.materials.push_back(std::move(out));
    }
}

} // namespace

std::string model_material_path(const std::filesystem::path& file, std::size_t index) {
    return file.generic_string() + "#material/" + std::to_string(index);
}

std::string model_image_path(const std::filesystem::path& file, std::size_t index) {
    return file.generic_string() + "#image/" + std::to_string(index);
}

std::string gltf_mesh_path(const std::filesystem::path& file, std::size_t index) {
    return file.generic_string() + "#mesh/" + std::to_string(index);
}

GltfSceneLoadResult load_gltf_scene(const std::filesystem::path& path) {
    F3D_ZONE;
    GltfSceneLoadResult result;
    auto gltf = parse(path, fastgltf::Options::DecomposeNodeMatrices, result.error);
    if (!gltf) {
        return result;
    }
    GltfScene scene;
    scene.name = path.stem().string();
    // External images are dependencies (buffers were loaded already and are part of the file).
    for (const fastgltf::Image& image : gltf->images) {
        if (const auto* uri = std::get_if<fastgltf::sources::URI>(&image.data); uri && uri->uri.isLocalPath()) {
            scene.dependencies.push_back(uri->uri.fspath().generic_string());
        }
    }

    std::size_t skipped = 0;
    std::size_t primitives = 0;
    scene.meshes.resize(gltf->meshes.size());
    for (std::size_t i = 0; i < gltf->meshes.size(); ++i) {
        MeshData& mesh = scene.meshes[i];
        mesh.name = gltf->meshes[i].name.empty() ? scene.name + " mesh " + std::to_string(i)
                                                 : std::string(gltf->meshes[i].name);
        skipped += append_mesh(*gltf, i, glm::mat4(1.0f), mesh, false);
        primitives += mesh.primitive_count;
    }
    if (primitives == 0) {
        result.error = path.filename().string() + ": no triangle meshes";
        return result;
    }
    read_materials(*gltf, scene);

    // Depth-first over the default scene (or every node, flat, when there is no scene).
    const bool has_scene = !gltf->scenes.empty();
    std::vector<std::size_t> roots;
    if (has_scene) {
        const auto& s = gltf->scenes[gltf->defaultScene.value_or(0)];
        roots.assign(s.nodeIndices.begin(), s.nodeIndices.end());
    } else {
        for (std::size_t i = 0; i < gltf->nodes.size(); ++i) {
            roots.push_back(i);
        }
    }
    struct Item {
        std::size_t node;
        int parent;
    };
    std::vector<Item> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        stack.push_back({*it, -1});
    }
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const fastgltf::Node& node = gltf->nodes[item.node];
        GltfNode out;
        out.name = node.name.empty() ? "Node " + std::to_string(item.node) : std::string(node.name);
        out.parent = item.parent;
        if (node.meshIndex && scene.meshes[*node.meshIndex].primitive_count > 0) {
            out.mesh = static_cast<int>(*node.meshIndex);
        }
        if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
            out.translation = {trs->translation.x(), trs->translation.y(), trs->translation.z()};
            out.rotation = glm::quat(trs->rotation.w(), trs->rotation.x(), trs->rotation.y(), trs->rotation.z());
            out.scale = {trs->scale.x(), trs->scale.y(), trs->scale.z()};
        }
        const int index = static_cast<int>(scene.nodes.size());
        scene.nodes.push_back(std::move(out));
        if (has_scene) {
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
                stack.push_back({*it, index});
            }
        }
    }
    if (skipped > 0) {
        F3D_LOG_WARN("{}: skipped {} non-triangle primitives", path.filename().string(), skipped);
    }
    F3D_LOG_INFO("Loaded {}: {} nodes, {} meshes, {} primitives", path.filename().string(), scene.nodes.size(),
                 scene.meshes.size(), primitives);
    result.scene = std::move(scene);
    return result;
}

} // namespace f3d::assets
