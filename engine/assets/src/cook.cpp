// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/cook.hpp"

#include "f3d/core/profile.hpp"

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>
#include <xatlas.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace f3d::assets {
namespace {

constexpr std::array<char, 4> kMagic{'F', '3', 'D', 'M'};
constexpr std::uint32_t kVersion = 2; // 2: submesh table per LOD (P4.2)
constexpr std::uint32_t kFlagUvs = 1;

class Writer {
public:
    void u32(std::uint32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void raw(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    }
    std::vector<std::uint8_t> out;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}
    bool raw(void* dst, std::size_t size) {
        if (pos_ + size > data_.size()) {
            return false;
        }
        std::memcpy(dst, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }
    bool u32(std::uint32_t& v) { return raw(&v, 4); }
    bool f32(float& v) { return raw(&v, 4); }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

/// Reorders vertices and indices for the GPU's vertex cache and fetch. Triangles
/// stay inside their submesh.
void optimize(MeshData& mesh, const std::vector<SubMesh>& submeshes) {
    for (const SubMesh& s : submeshes) {
        std::uint32_t* range = mesh.indices.data() + s.first_index;
        meshopt_optimizeVertexCache(range, range, s.index_count, mesh.vertices.size());
    }
    std::vector<unsigned int> remap(mesh.vertices.size());
    const std::size_t unique =
        meshopt_optimizeVertexFetchRemap(remap.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.size());
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
}

} // namespace

MeshCookSettings MeshCookSettings::from_json(const nlohmann::json& json) {
    MeshCookSettings s;
    if (!json.is_object()) {
        return s;
    }
    s.max_lods = std::clamp<std::uint32_t>(json.value("max_lods", s.max_lods), 1, 8);
    s.lod_ratio = std::clamp(json.value("lod_ratio", s.lod_ratio), 0.05f, 0.95f);
    s.lod_error = std::clamp(json.value("lod_error", s.lod_error), 0.0f, 1.0f);
    s.generate_uvs = json.value("generate_uvs", s.generate_uvs);
    return s;
}

nlohmann::json MeshCookSettings::to_json() const {
    return {{"max_lods", max_lods}, {"lod_ratio", lod_ratio}, {"lod_error", lod_error}, {"generate_uvs", generate_uvs}};
}

std::vector<SubMesh> CookedMesh::lod_submeshes(std::size_t lod) const {
    if (lod >= lods.size()) {
        return {};
    }
    const MeshLod& level = lods[lod];
    if (level.submeshes.empty()) {
        return {SubMesh{0, level.index_count, SubMesh::kNoMaterial}};
    }
    std::vector<SubMesh> out = level.submeshes;
    for (SubMesh& s : out) {
        s.first_index -= level.first_index;
    }
    return out;
}

MeshData CookedMesh::to_mesh_data(std::size_t lod) const {
    MeshData mesh;
    mesh.name = name;
    mesh.vertices = vertices;
    mesh.uvs = uvs;
    mesh.bounds = bounds;
    if (lod < lods.size()) {
        const auto first = indices.begin() + lods[lod].first_index;
        mesh.indices.assign(first, first + lods[lod].index_count);
        if (!lods[lod].submeshes.empty()) {
            mesh.submeshes = lod_submeshes(lod);
        }
    }
    mesh.primitive_count = mesh.indices.empty() ? 0 : 1;
    return mesh;
}

bool generate_uvs(MeshData& mesh, std::string* error) {
    F3D_ZONE;
    std::unique_ptr<xatlas::Atlas, decltype(&xatlas::Destroy)> atlas(xatlas::Create(), &xatlas::Destroy);
    xatlas::MeshDecl decl;
    decl.vertexPositionData = &mesh.vertices[0].position;
    decl.vertexPositionStride = sizeof(MeshVertex);
    decl.vertexNormalData = &mesh.vertices[0].normal;
    decl.vertexNormalStride = sizeof(MeshVertex);
    decl.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    decl.indexData = mesh.indices.data();
    decl.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    const xatlas::AddMeshError added = xatlas::AddMesh(atlas.get(), decl);
    if (added != xatlas::AddMeshError::Success) {
        if (error) {
            *error = std::string("xatlas: ") + xatlas::StringForEnum(added);
        }
        return false;
    }
    xatlas::Generate(atlas.get());
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        if (error) {
            *error = "xatlas produced no atlas";
        }
        return false;
    }
    const xatlas::Mesh& out = atlas->meshes[0];
    MeshData result;
    result.name = mesh.name;
    result.primitive_count = mesh.primitive_count;
    result.bounds = mesh.bounds;
    result.vertices.resize(out.vertexCount);
    result.uvs.resize(out.vertexCount);
    for (std::uint32_t i = 0; i < out.vertexCount; ++i) {
        const xatlas::Vertex& v = out.vertexArray[i];
        result.vertices[i] = mesh.vertices[v.xref];
        result.uvs[i] = {v.uv[0] / static_cast<float>(atlas->width), v.uv[1] / static_cast<float>(atlas->height)};
    }
    result.indices.assign(out.indexArray, out.indexArray + out.indexCount);
    result.submeshes = mesh.submeshes; // xatlas keeps the triangle order
    mesh = std::move(result);
    return true;
}

CookedMesh cook_mesh(MeshData mesh, const MeshCookSettings& settings) {
    F3D_ZONE;
    CookedMesh out;
    out.name = mesh.name;
    if (settings.generate_uvs && !mesh.indices.empty()) {
        generate_uvs(mesh);
    }
    if (mesh.indices.empty() || mesh.vertices.empty()) {
        out.bounds = mesh.bounds;
        return out;
    }
    const bool has_submeshes = !mesh.submeshes.empty();
    const std::vector<SubMesh> submeshes = submeshes_of(mesh);
    optimize(mesh, submeshes);
    out.bounds = {};
    for (const MeshVertex& v : mesh.vertices) {
        out.bounds.add(v.position);
    }

    out.indices = mesh.indices;
    out.lods.push_back(
        {0, static_cast<std::uint32_t>(mesh.indices.size()), 0.0f, has_submeshes ? submeshes : std::vector<SubMesh>{}});
    // Each level simplifies the previous one, aiming for lod_ratio of its triangles.
    // Submeshes simplify separately so every triangle keeps its material.
    std::vector<SubMesh> previous = submeshes; // relative to `previous_indices`
    std::vector<std::uint32_t> previous_indices = mesh.indices;
    for (std::uint32_t level = 1; level < settings.max_lods; ++level) {
        const std::size_t previous_count = previous_indices.size();
        if (static_cast<std::size_t>(static_cast<float>(previous_count / 3) * settings.lod_ratio) * 3 < 36) {
            break; // too small to bother
        }
        std::vector<std::uint32_t> lod;
        std::vector<SubMesh> lod_submeshes;
        float error = 0.0f;
        for (const SubMesh& s : previous) {
            const std::uint32_t* source = previous_indices.data() + s.first_index;
            const auto target =
                static_cast<std::size_t>(static_cast<float>(s.index_count / 3) * settings.lod_ratio) * 3;
            std::vector<std::uint32_t> part(s.index_count);
            std::size_t count = s.index_count;
            if (target >= 3) {
                float part_error = 0.0f;
                count = meshopt_simplify(part.data(), source, s.index_count, &mesh.vertices[0].position.x,
                                         mesh.vertices.size(), sizeof(MeshVertex), target, settings.lod_error, 0,
                                         &part_error);
                error = std::max(error, part_error);
            }
            if (count == 0 || target < 3) {
                part.assign(source, source + s.index_count); // too small to simplify: keep it
                count = s.index_count;
            }
            part.resize(count);
            meshopt_optimizeVertexCache(part.data(), part.data(), part.size(), mesh.vertices.size());
            lod_submeshes.push_back(
                {static_cast<std::uint32_t>(lod.size()), static_cast<std::uint32_t>(count), s.material});
            lod.insert(lod.end(), part.begin(), part.end());
        }
        if (lod.empty() || lod.size() > previous_count * 9 / 10) {
            break; // no meaningful reduction within the error budget
        }
        const auto first = static_cast<std::uint32_t>(out.indices.size());
        std::vector<SubMesh> absolute;
        if (has_submeshes) {
            absolute = lod_submeshes;
            for (SubMesh& s : absolute) {
                s.first_index += first;
            }
        }
        out.lods.push_back({first, static_cast<std::uint32_t>(lod.size()), error, std::move(absolute)});
        out.indices.insert(out.indices.end(), lod.begin(), lod.end());
        previous = std::move(lod_submeshes);
        previous_indices = std::move(lod);
    }
    out.vertices = std::move(mesh.vertices);
    out.uvs = std::move(mesh.uvs);
    return out;
}

std::vector<std::uint8_t> write_f3dmesh(const CookedMesh& mesh) {
    Writer w;
    w.raw(kMagic.data(), kMagic.size());
    w.u32(kVersion);
    w.u32(mesh.uvs.empty() ? 0 : kFlagUvs);
    w.u32(static_cast<std::uint32_t>(mesh.vertices.size()));
    w.u32(static_cast<std::uint32_t>(mesh.indices.size()));
    w.u32(static_cast<std::uint32_t>(mesh.lods.size()));
    for (int i = 0; i < 3; ++i) {
        w.f32(mesh.bounds.min[i]);
    }
    for (int i = 0; i < 3; ++i) {
        w.f32(mesh.bounds.max[i]);
    }
    w.u32(static_cast<std::uint32_t>(mesh.name.size()));
    w.raw(mesh.name.data(), mesh.name.size());
    for (const MeshLod& lod : mesh.lods) {
        w.u32(lod.first_index);
        w.u32(lod.index_count);
        w.f32(lod.error);
    }
    for (const MeshLod& lod : mesh.lods) {
        w.u32(static_cast<std::uint32_t>(lod.submeshes.size()));
        for (const SubMesh& s : lod.submeshes) {
            w.u32(s.first_index);
            w.u32(s.index_count);
            w.u32(s.material);
        }
    }
    static_assert(sizeof(MeshVertex) == 28);
    w.raw(mesh.vertices.data(), mesh.vertices.size() * sizeof(MeshVertex));
    if (!mesh.uvs.empty()) {
        w.raw(mesh.uvs.data(), mesh.uvs.size() * sizeof(glm::vec2));
    }
    w.raw(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
    return std::move(w.out);
}

std::optional<CookedMesh> read_f3dmesh(std::span<const std::uint8_t> data, std::string* error) {
    const auto fail = [&](const char* why) -> std::optional<CookedMesh> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    Reader r(data);
    std::array<char, 4> magic{};
    std::uint32_t version = 0, flags = 0, vertex_count = 0, index_count = 0, lod_count = 0, name_length = 0;
    if (!r.raw(magic.data(), 4) || magic != kMagic) {
        return fail("not an .f3dmesh file");
    }
    if (!r.u32(version) || version < 1 || version > kVersion) {
        return fail("unsupported .f3dmesh version");
    }
    CookedMesh mesh;
    if (!r.u32(flags) || !r.u32(vertex_count) || !r.u32(index_count) || !r.u32(lod_count)) {
        return fail("truncated header");
    }
    for (int i = 0; i < 3; ++i) {
        r.f32(mesh.bounds.min[i]);
    }
    for (int i = 0; i < 3; ++i) {
        r.f32(mesh.bounds.max[i]);
    }
    if (!r.u32(name_length) || name_length > 4096 || lod_count > 16) {
        return fail("corrupt header");
    }
    const std::size_t streams = vertex_count * sizeof(MeshVertex) +
                                ((flags & kFlagUvs) ? vertex_count * sizeof(glm::vec2) : 0) +
                                index_count * sizeof(std::uint32_t);
    if (r.remaining() < name_length + lod_count * 12ull + streams) {
        return fail("size mismatch");
    }
    mesh.name.resize(name_length);
    r.raw(mesh.name.data(), name_length);
    mesh.lods.resize(lod_count);
    for (MeshLod& lod : mesh.lods) {
        r.u32(lod.first_index);
        r.u32(lod.index_count);
        r.f32(lod.error);
        if (static_cast<std::uint64_t>(lod.first_index) + lod.index_count > index_count || lod.index_count % 3 != 0) {
            return fail("corrupt LOD table");
        }
    }
    if (version >= 2) {
        for (MeshLod& lod : mesh.lods) {
            std::uint32_t count = 0;
            if (!r.u32(count) || count > 4096) {
                return fail("corrupt submesh table");
            }
            lod.submeshes.resize(count);
            for (SubMesh& s : lod.submeshes) {
                if (!r.u32(s.first_index) || !r.u32(s.index_count) || !r.u32(s.material) ||
                    s.first_index < lod.first_index ||
                    static_cast<std::uint64_t>(s.first_index) + s.index_count > lod.first_index + lod.index_count ||
                    s.index_count % 3 != 0) {
                    return fail("corrupt submesh table");
                }
            }
        }
    }
    if (r.remaining() != streams) {
        return fail("size mismatch");
    }
    mesh.vertices.resize(vertex_count);
    r.raw(mesh.vertices.data(), vertex_count * sizeof(MeshVertex));
    if (flags & kFlagUvs) {
        mesh.uvs.resize(vertex_count);
        r.raw(mesh.uvs.data(), vertex_count * sizeof(glm::vec2));
    }
    mesh.indices.resize(index_count);
    r.raw(mesh.indices.data(), index_count * sizeof(std::uint32_t));
    if (std::any_of(mesh.indices.begin(), mesh.indices.end(), [&](std::uint32_t i) { return i >= vertex_count; })) {
        return fail("index out of range");
    }
    return mesh;
}

} // namespace f3d::assets
