// SPDX-License-Identifier: Apache-2.0
#include "f3d/io/world_chunks.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/scatter_paint.hpp"

#include <cstring>

namespace f3d::io {
namespace {

constexpr std::uint32_t kHeightsMagic = 0x46483346u; // "F3HF"
constexpr std::uint32_t kSplatMagic = 0x50533346u;   // "F3SP"
constexpr std::uint32_t kVersion = 1;

void put(Bytes& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

bool get(const Bytes& in, std::size_t& at, std::uint32_t& value) {
    if (at + 4 > in.size()) {
        return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(in[at + i]) << (8 * i);
    }
    at += 4;
    return true;
}

std::string terrain_chunk(const core::Guid& guid, const char* what) {
    return "terrain/" + guid.to_string() + "/" + what;
}

} // namespace

std::map<std::string, Bytes> world_chunks(scene::World& world) {
    F3D_ZONE;
    std::map<std::string, Bytes> out;
    for (const entt::entity e : world.registry().view<terrain::Terrain>()) {
        const auto hf = terrain::heightfield_of(world, e);
        if (!hf) {
            continue;
        }
        const core::Guid guid = world.guid(e);
        Bytes& heights = out[terrain_chunk(guid, "heights")];
        put(heights, kHeightsMagic);
        put(heights, kVersion);
        put(heights, hf->samples());
        const std::size_t base = heights.size();
        heights.resize(base + hf->heights().size() * sizeof(float));
        std::memcpy(heights.data() + base, hf->heights().data(), hf->heights().size() * sizeof(float));
        if (const terrain::Splatmap* splat = hf->splat()) {
            Bytes& weights = out[terrain_chunk(guid, "splat")];
            put(weights, kSplatMagic);
            put(weights, kVersion);
            put(weights, splat->tiles());
            put(weights, splat->maps());
            for (std::uint32_t m = 0; m < splat->maps(); ++m) {
                weights.insert(weights.end(), splat->map(m), splat->map(m) + splat->map_bytes());
            }
        }
    }
    for (auto& [name, bytes] : world::scatter_chunks(world)) {
        out[name] = std::move(bytes);
    }
    return out;
}

void restore_world_chunks(scene::World& world, const std::map<std::string, Bytes>& chunks) {
    F3D_ZONE;
    for (const entt::entity e : world.registry().view<terrain::Terrain>()) {
        const terrain::Terrain& t = world.registry().get<terrain::Terrain>(e);
        const core::Guid guid = world.guid(e);
        const auto heights = chunks.find(terrain_chunk(guid, "heights"));
        if (heights == chunks.end()) {
            continue;
        }
        const Bytes& data = heights->second;
        std::size_t at = 0;
        std::uint32_t magic = 0, version = 0, samples = 0;
        auto hf = std::make_shared<terrain::Heightfield>(t.tiles, t.size);
        if (!get(data, at, magic) || !get(data, at, version) || !get(data, at, samples) || magic != kHeightsMagic ||
            version != kVersion || samples != hf->samples() ||
            data.size() - at != static_cast<std::size_t>(samples) * samples * sizeof(float)) {
            F3D_LOG_WARN("Terrain {}: its saved heights don't match it; skipped", world.name(e));
            continue;
        }
        std::memcpy(hf->heights().data(), data.data() + at, data.size() - at);
        hf->update_bounds();
        hf->mark_all_dirty();
        if (const auto splat = chunks.find(terrain_chunk(guid, "splat")); splat != chunks.end()) {
            const Bytes& w = splat->second;
            std::size_t p = 0;
            std::uint32_t smagic = 0, sversion = 0, tiles = 0, maps = 0;
            if (get(w, p, smagic) && get(w, p, sversion) && get(w, p, tiles) && get(w, p, maps) &&
                smagic == kSplatMagic && sversion == kVersion && tiles == hf->tiles() && maps >= 1 &&
                maps <= terrain::kMaxLayers / 4) {
                terrain::Splatmap& map = hf->ensure_splat(maps * 4);
                if (w.size() - p == map.map_bytes() * maps) {
                    for (std::uint32_t m = 0; m < maps; ++m) {
                        std::memcpy(map.map(m), w.data() + p + m * map.map_bytes(), map.map_bytes());
                    }
                    map.mark_all_dirty();
                } else {
                    F3D_LOG_WARN("Terrain {}: its saved layer weights don't match it; skipped", world.name(e));
                }
            }
        }
        terrain::heightfields(world).set(guid, std::move(hf));
    }
    world::restore_scatter_chunks(world, chunks);
}

} // namespace f3d::io
