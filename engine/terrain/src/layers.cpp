// SPDX-License-Identifier: Apache-2.0
#include "f3d/terrain/layers.hpp"

#include "f3d/terrain/builtin_textures.hpp"

#include "f3d/core/jobs.hpp"
#include "f3d/core/profile.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace f3d::terrain {
namespace {

constexpr std::uint32_t kTile = kSplatPerTile;

std::size_t block_bytes(std::uint32_t maps) noexcept {
    return static_cast<std::size_t>(maps) * kTile * kTile * 4;
}

/// A tile's texels in the first `maps` maps: map by map, row by row.
std::vector<std::uint8_t> read_block(const Splatmap& splat, std::uint32_t tile, std::uint32_t maps) {
    const std::uint32_t res = splat.resolution();
    const std::uint32_t tx = (tile % splat.tiles()) * kTile;
    const std::uint32_t tz = (tile / splat.tiles()) * kTile;
    std::vector<std::uint8_t> out(block_bytes(maps));
    std::uint8_t* dst = out.data();
    for (std::uint32_t m = 0; m < maps; ++m) {
        const std::uint8_t* src = splat.map(m);
        for (std::uint32_t z = 0; z < kTile; ++z) {
            std::memcpy(dst, src + (static_cast<std::size_t>(tz + z) * res + tx) * 4, kTile * 4);
            dst += kTile * 4;
        }
    }
    return out;
}

void write_block(Splatmap& splat, std::uint32_t tile, std::uint32_t maps, const std::vector<std::uint8_t>& block) {
    splat.ensure_maps(maps);
    const std::uint32_t res = splat.resolution();
    const std::uint32_t tx = (tile % splat.tiles()) * kTile;
    const std::uint32_t tz = (tile / splat.tiles()) * kTile;
    const std::uint8_t* src = block.data();
    for (std::uint32_t m = 0; m < maps; ++m) {
        std::uint8_t* dst = splat.map(m);
        for (std::uint32_t z = 0; z < kTile; ++z) {
            std::memcpy(dst + (static_cast<std::size_t>(tz + z) * res + tx) * 4, src, kTile * 4);
            src += kTile * 4;
        }
    }
    splat.mark_dirty(tile);
}

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(data.size()));
    const std::size_t written = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 1);
    out.resize(ZSTD_isError(written) ? 0 : written);
    return out;
}

std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& data, std::size_t size) {
    std::vector<std::uint8_t> out(size, 0);
    const std::size_t read = ZSTD_decompress(out.data(), size, data.data(), data.size());
    if (ZSTD_isError(read) || read != size) {
        std::fill(out.begin(), out.end(), std::uint8_t{0});
    }
    return out;
}

std::vector<std::uint8_t> xor_bytes(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::vector<std::uint8_t> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return out;
}

using Weights = std::array<float, kMaxLayers>;

Weights read_weights(const Splatmap& splat, std::uint32_t x, std::uint32_t z) {
    Weights w{};
    float sum = 0.0f;
    for (std::uint32_t l = 0; l < splat.maps() * 4; ++l) {
        w[l] = static_cast<float>(splat.weight(x, z, l)) / 255.0f;
        sum += w[l];
    }
    if (sum <= 0.0f) {
        w[0] = 1.0f;
    }
    return w;
}

/// Bytes summing to 255 (largest remainders get the rounding); all-zero weights give layer 0.
void write_weights(Splatmap& splat, std::uint32_t x, std::uint32_t z, const Weights& w) {
    const std::uint32_t layers = splat.maps() * 4;
    float total = 0.0f;
    for (std::uint32_t l = 0; l < layers; ++l) {
        total += std::max(w[l], 0.0f);
    }
    std::array<std::uint32_t, kMaxLayers> bytes{};
    std::array<float, kMaxLayers> rest{};
    if (total <= 0.0f) {
        bytes[0] = 255;
    } else {
        std::uint32_t used = 0;
        for (std::uint32_t l = 0; l < layers; ++l) {
            const float scaled = std::max(w[l], 0.0f) / total * 255.0f;
            bytes[l] = static_cast<std::uint32_t>(scaled);
            rest[l] = scaled - static_cast<float>(bytes[l]);
            used += bytes[l];
        }
        for (; used < 255; ++used) {
            std::uint32_t best = 0;
            for (std::uint32_t l = 1; l < layers; ++l) {
                if (rest[l] > rest[best]) {
                    best = l;
                }
            }
            ++bytes[best];
            rest[best] = -1.0f;
        }
    }
    for (std::uint32_t l = 0; l < layers; ++l) {
        splat.set_weight(x, z, l, static_cast<std::uint8_t>(bytes[l]));
    }
}

float band(float value, float low, float high, float falloff) noexcept {
    if (falloff <= 0.0f) {
        return value >= low && value <= high ? 1.0f : 0.0f;
    }
    const auto smooth = [](float e0, float e1, float v) {
        const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float half = falloff * 0.5f;
    return smooth(low - half, low + half, value) * (1.0f - smooth(high - half, high + half, value));
}

/// A file name's stem split into lower-case words ("Rock_NormalGL-2k" -> rock, normalgl, 2k).
std::vector<std::string> stem_words(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t hash = name.find('#');
    name = name.substr(0, hash);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    std::vector<std::string> words(1);
    for (const char c : name) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            if (!words.back().empty()) {
                words.emplace_back();
            }
        } else {
            words.back().push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (words.back().empty()) {
        words.pop_back();
    }
    return words;
}

std::optional<LayerTextureSlot> word_slot(const std::string& w) {
    const auto starts = [&](const char* prefix) { return w.rfind(prefix, 0) == 0; };
    if (starts("normal") || w == "nrm" || w == "nor" || w == "norm" || w == "n") {
        return LayerTextureSlot::Normal;
    }
    if (starts("rough") || w == "rgh") {
        return LayerTextureSlot::Roughness;
    }
    if (w == "ao" || starts("ambientocclusion") || starts("occlusion") || w == "occ") {
        return LayerTextureSlot::Ao;
    }
    if (starts("height") || starts("disp") || w == "bump" || w == "depth") {
        return LayerTextureSlot::Height;
    }
    if (starts("albedo") || starts("basecolor") || starts("color") || starts("colour") || starts("diff") ||
        w == "col" || w == "base") {
        return LayerTextureSlot::Albedo;
    }
    return std::nullopt;
}

bool is_packed_word(const std::string& w) {
    return w == "arm" || w == "orm";
}

/// "2k", "4K", "2048": a resolution tag, not part of a texture's name.
bool is_resolution_word(const std::string& w) {
    if (w.size() >= 2 && w.back() == 'k' &&
        std::all_of(w.begin(), w.end() - 1, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return true;
    }
    return w == "512" || w == "1024" || w == "2048" || w == "4096" || w == "8192";
}

/// The slot word of a file name (the last word naming a slot, else a packed "arm"
/// word) and the other words joined.
std::pair<std::optional<LayerTextureSlot>, std::string> split_slot(std::string_view path) {
    std::vector<std::string> words = stem_words(path);
    std::optional<LayerTextureSlot> slot;
    std::size_t at = words.size();
    for (std::size_t i = words.size(); i-- > 0;) {
        if (const auto s = word_slot(words[i])) {
            slot = s;
            at = i;
            break;
        }
    }
    for (std::size_t i = words.size(); !slot && i-- > 0;) {
        if (is_packed_word(words[i])) {
            at = i;
            break;
        }
    }
    std::string base;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i == at || words[i] == "gl" || words[i] == "dx" || words[i] == "ogl") {
            continue; // the slot word, and normal-map conventions ("nor_gl")
        }
        if (!base.empty()) {
            base += '_';
        }
        base += words[i];
    }
    // "Color" in "basecolor" style pairs ("base", "color") leaves "base": drop it too.
    if (slot == LayerTextureSlot::Albedo && base.size() >= 4 && base.ends_with("base")) {
        base.resize(base.size() - 4);
        if (!base.empty() && base.back() == '_') {
            base.pop_back();
        }
    }
    return {slot, base};
}

std::string_view folder_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

} // namespace

// --- Texture sets ----------------------------------------------------------------------

core::AssetRef& layer_texture(TerrainLayer& layer, LayerTextureSlot slot) noexcept {
    switch (slot) {
    case LayerTextureSlot::Normal: return layer.normal_texture;
    case LayerTextureSlot::Roughness: return layer.roughness_texture;
    case LayerTextureSlot::Ao: return layer.ao_texture;
    case LayerTextureSlot::Height: return layer.height_texture;
    case LayerTextureSlot::Albedo: break;
    }
    return layer.albedo_texture;
}

const core::AssetRef& layer_texture(const TerrainLayer& layer, LayerTextureSlot slot) noexcept {
    return layer_texture(const_cast<TerrainLayer&>(layer), slot);
}

std::optional<LayerTextureSlot> guess_texture_slot(std::string_view path) {
    return split_slot(path).first;
}

bool is_packed_orm(std::string_view path) {
    const std::vector<std::string> words = stem_words(path);
    return !split_slot(path).first && std::any_of(words.begin(), words.end(), is_packed_word);
}

std::string texture_set_name(std::string_view path) {
    const std::string base = split_slot(path).second;
    std::string name;
    std::size_t start = 0;
    while (start <= base.size()) {
        const std::size_t end = std::min(base.find('_', start), base.size());
        const std::string word = base.substr(start, end - start);
        if (!word.empty() && !is_resolution_word(word)) {
            name += (name.empty() ? "" : " ") + word;
        }
        start = end + 1;
    }
    if (name.empty()) {
        return "Layer";
    }
    name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
    return name;
}

std::array<std::string, kLayerTextureSlots> guess_texture_set(std::string_view path,
                                                               const std::vector<std::string>& siblings) {
    std::array<std::string, kLayerTextureSlots> out;
    const auto [own_slot, base] = split_slot(path);
    const LayerTextureSlot slot = own_slot.value_or(LayerTextureSlot::Albedo);
    out[static_cast<std::size_t>(slot)] = std::string(path);
    const std::string_view folder = folder_of(path);
    std::string packed;
    for (const std::string& other : siblings) {
        if (other == path || folder_of(other) != folder) {
            continue;
        }
        const auto [other_slot, other_base] = split_slot(other);
        if (!other_slot && other_base == base && is_packed_orm(other)) {
            packed = other;
            continue;
        }
        if (!other_slot || other_base != base || *other_slot == slot) {
            continue;
        }
        std::string& entry = out[static_cast<std::size_t>(*other_slot)];
        // Prefer OpenGL-style normal maps (forge3d's convention) over DirectX ones.
        const std::vector<std::string> words = stem_words(other);
        const bool directx =
            std::any_of(words.begin(), words.end(), [](const std::string& w) { return w.ends_with("dx"); });
        if (entry.empty() || (*other_slot == LayerTextureSlot::Normal && !directx)) {
            entry = other;
        }
    }
    // A packed map stands in for the separate roughness and AO maps the set lacks.
    for (const LayerTextureSlot s : {LayerTextureSlot::Roughness, LayerTextureSlot::Ao}) {
        if (!packed.empty() && out[static_cast<std::size_t>(s)].empty()) {
            out[static_cast<std::size_t>(s)] = packed;
        }
    }
    return out;
}

// --- Splatmap --------------------------------------------------------------------------

Splatmap::Splatmap(std::uint32_t tiles, std::uint32_t maps)
    : tiles_(std::max(tiles, 1u)), maps_(std::clamp(maps, 1u, kMaxLayers / 4)),
      dirty_tiles_(static_cast<std::size_t>(tiles_) * tiles_, true),
      tile_revisions_(static_cast<std::size_t>(tiles_) * tiles_, 1) {
    data_.assign(maps_ * map_bytes(), 0);
}

void Splatmap::ensure_maps(std::uint32_t maps) {
    maps = std::clamp(maps, 1u, kMaxLayers / 4);
    if (maps <= maps_) {
        return;
    }
    data_.resize(maps * map_bytes(), 0);
    maps_ = maps;
    mark_all_dirty();
}

std::uint8_t Splatmap::weight(std::uint32_t x, std::uint32_t z, std::uint32_t layer) const noexcept {
    const std::uint32_t m = layer / 4;
    if (m >= maps_) {
        return 0;
    }
    return map(m)[(static_cast<std::size_t>(z) * resolution() + x) * 4 + layer % 4];
}

void Splatmap::set_weight(std::uint32_t x, std::uint32_t z, std::uint32_t layer, std::uint8_t value) noexcept {
    const std::uint32_t m = layer / 4;
    if (m < maps_) {
        map(m)[(static_cast<std::size_t>(z) * resolution() + x) * 4 + layer % 4] = value;
    }
}

void Splatmap::mark_dirty(std::uint32_t tile) noexcept {
    if (tile < dirty_tiles_.size()) {
        dirty_tiles_[tile] = true;
        tile_revisions_[tile] = revision_ + 1;
    }
    ++revision_;
}

void Splatmap::mark_all_dirty() noexcept {
    std::fill(dirty_tiles_.begin(), dirty_tiles_.end(), true);
    ++revision_;
    std::fill(tile_revisions_.begin(), tile_revisions_.end(), revision_);
}

std::vector<std::uint32_t> Splatmap::take_dirty_tiles() {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < dirty_tiles_.size(); ++i) {
        if (dirty_tiles_[i]) {
            out.push_back(i);
            dirty_tiles_[i] = false;
        }
    }
    return out;
}

Splatmap& Heightfield::ensure_splat(std::uint32_t layers) {
    const std::uint32_t maps = std::clamp((std::max(layers, 1u) + 3) / 4, 1u, kMaxLayers / 4);
    if (!splat_) {
        splat_ = std::make_unique<Splatmap>(tiles_, maps);
        std::uint8_t* base = splat_->map(0);
        for (std::size_t i = 0; i < splat_->map_bytes(); i += 4) {
            base[i] = 255; // layer 0 everywhere
        }
    } else {
        splat_->ensure_maps(maps);
    }
    return *splat_;
}

std::vector<TerrainLayer> default_layers(float min_height, float max_height) {
    std::vector<TerrainLayer> layers = {builtin_layer("grass"), builtin_layer("rock"), builtin_layer("snow")};
    layers[1].rule = {true, -1.0e4f, 1.0e4f, 35.0f, 90.0f, 8.0f};
    const float snow_line = min_height + (max_height - min_height) * 0.85f;
    layers[2].rule = {true, snow_line, 1.0e4f, 0.0f, 40.0f, std::max((max_height - min_height) * 0.03f, 1.0f)};
    return layers;
}

// --- Rules -----------------------------------------------------------------------------

float rule_weight(const LayerRule& rule, float height, float slope_degrees) noexcept {
    if (!rule.enabled) {
        return 0.0f;
    }
    return band(height, rule.height_min, rule.height_max, rule.falloff) *
           band(slope_degrees, rule.slope_min, rule.slope_max, rule.falloff);
}

void apply_rules(const Heightfield& heights, const Terrain& terrain, Splatmap& splat) {
    F3D_ZONE;
    const auto layers = static_cast<std::uint32_t>(std::min<std::size_t>(terrain.layers.size(), kMaxLayers));
    splat.ensure_maps((std::max(layers, 1u) + 3) / 4);
    const std::uint32_t res = splat.resolution();
    const float texel = heights.size() / static_cast<float>(res);
    const float half = heights.size() * 0.5f;
    core::jobs::parallel_for(
        res,
        [&](std::size_t begin, std::size_t end) {
            for (auto z = static_cast<std::uint32_t>(begin); z < end; ++z) {
                for (std::uint32_t x = 0; x < res; ++x) {
                    const float lx = (static_cast<float>(x) + 0.5f) * texel - half;
                    const float lz = (static_cast<float>(z) + 0.5f) * texel - half;
                    const float h = heights.height_at(lx, lz);
                    const float slope = std::acos(std::clamp(heights.normal_at(lx, lz).y, -1.0f, 1.0f)) * 57.29578f;
                    Weights w{};
                    w[0] = 1.0f;
                    for (std::uint32_t k = 1; k < layers; ++k) {
                        const float a = rule_weight(terrain.layers[k].rule, h, slope);
                        if (a <= 0.0f) {
                            continue;
                        }
                        for (std::uint32_t j = 0; j < k; ++j) {
                            w[j] *= 1.0f - a;
                        }
                        w[k] = a;
                    }
                    write_weights(splat, x, z, w);
                }
            }
        },
        16);
    splat.mark_all_dirty();
}

void paint_texel(Splatmap& splat, std::uint32_t x, std::uint32_t z, std::uint32_t layer, float amount) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    if (a <= 0.0f || layer >= splat.maps() * 4) {
        return;
    }
    Weights w = read_weights(splat, x, z);
    for (std::uint32_t j = 0; j < splat.maps() * 4; ++j) {
        w[j] *= 1.0f - a;
    }
    w[layer] += a;
    write_weights(splat, x, z, w);
}

void remap_splat(Splatmap& splat, const std::vector<std::int32_t>& source) {
    F3D_ZONE;
    const auto layers = static_cast<std::uint32_t>(std::min<std::size_t>(source.size(), kMaxLayers));
    splat.ensure_maps((std::max(layers, 1u) + 3) / 4);
    const std::uint32_t res = splat.resolution();
    core::jobs::parallel_for(
        res,
        [&](std::size_t begin, std::size_t end) {
            for (auto z = static_cast<std::uint32_t>(begin); z < end; ++z) {
                for (std::uint32_t x = 0; x < res; ++x) {
                    const Weights old = read_weights(splat, x, z);
                    Weights w{};
                    for (std::uint32_t i = 0; i < layers; ++i) {
                        const std::int32_t from = source[i];
                        w[i] = from >= 0 && from < static_cast<std::int32_t>(kMaxLayers) ? old[from] : 0.0f;
                    }
                    for (std::uint32_t l = layers; l < kMaxLayers; ++l) {
                        splat.set_weight(x, z, l, 0);
                    }
                    write_weights(splat, x, z, w); // normalises; nothing left: the base layer
                }
            }
        },
        16);
    splat.mark_all_dirty();
}

// --- Diffs -----------------------------------------------------------------------------

std::vector<std::vector<std::uint8_t>> splat_blocks(const Splatmap& splat) {
    std::vector<std::vector<std::uint8_t>> out;
    const std::uint32_t tiles = splat.tiles() * splat.tiles();
    out.reserve(tiles);
    for (std::uint32_t t = 0; t < tiles; ++t) {
        out.push_back(read_block(splat, t, splat.maps()));
    }
    return out;
}

SplatDiff diff_splat(const Splatmap& splat, const std::vector<std::vector<std::uint8_t>>& before) {
    F3D_ZONE;
    SplatDiff diff;
    diff.maps = splat.maps();
    for (std::uint32_t t = 0; t < before.size(); ++t) {
        std::vector<std::uint8_t> old = before[t];
        old.resize(block_bytes(diff.maps), 0); // maps added since: they were zero
        const std::vector<std::uint8_t> now = read_block(splat, t, diff.maps);
        if (old == now) {
            continue;
        }
        diff.tiles.push_back({t, compress(old), compress(xor_bytes(old, now))});
    }
    return diff;
}

void SplatDiff::apply_before(Splatmap& splat) const {
    for (const Tile& tile : tiles) {
        write_block(splat, tile.index, maps, decompress(tile.before, block_bytes(maps)));
    }
}

void SplatDiff::apply_after(Splatmap& splat) const {
    for (const Tile& tile : tiles) {
        const std::size_t size = block_bytes(maps);
        write_block(splat, tile.index, maps, xor_bytes(decompress(tile.before, size), decompress(tile.delta, size)));
    }
}

std::size_t SplatDiff::bytes() const noexcept {
    std::size_t total = 0;
    for (const Tile& tile : tiles) {
        total += sizeof(Tile) + tile.before.size() + tile.delta.size();
    }
    return total;
}

// --- Painting --------------------------------------------------------------------------

PaintRecorder::PaintRecorder(std::shared_ptr<Heightfield> heightfield, const Terrain& terrain, Brush brush,
                             std::uint32_t layer, bool erase)
    : heightfield_(std::move(heightfield)), brush_(std::move(brush)), layer_(std::min(layer, kMaxLayers - 1)),
      layer_count_(static_cast<std::uint32_t>(std::clamp<std::size_t>(terrain.layers.size(), 1, kMaxLayers))),
      erase_(erase), spacer_(std::max(brush_.radius, 1e-3f) * std::clamp(brush_.spacing, 0.01f, 4.0f)) {
    const Splatmap& splat = heightfield_->ensure_splat(std::max(layer_count_, layer_ + 1));
    slot_of_tile_.assign(static_cast<std::size_t>(splat.tiles()) * splat.tiles(), -1);
}

void PaintRecorder::add(const StrokePoint& point) {
    points_.push_back(point);
    dabs_.clear();
    spacer_.add(point, dabs_);
    for (const StrokePoint& dab : dabs_) {
        apply_dab(dab);
    }
}

void PaintRecorder::apply_dab(const StrokePoint& dab) {
    Splatmap& splat = *heightfield_->splat();
    const std::uint32_t res = splat.resolution();
    const float texel = heightfield_->size() / static_cast<float>(res);
    const float cx = (dab.position.x / heightfield_->size() + 0.5f) * static_cast<float>(res) - 0.5f;
    const float cz = (dab.position.y / heightfield_->size() + 0.5f) * static_cast<float>(res) - 0.5f;
    const float rt = brush_.radius / texel;
    if (!(rt > 0.0f)) {
        return;
    }
    const float last = static_cast<float>(res - 1);
    const float fx0 = std::max(std::ceil(cx - rt), 0.0f);
    const float fx1 = std::min(std::floor(cx + rt), last);
    const float fz0 = std::max(std::ceil(cz - rt), 0.0f);
    const float fz1 = std::min(std::floor(cz + rt), last);
    if (fx0 > fx1 || fz0 > fz1) {
        return;
    }
    const auto x0 = static_cast<std::uint32_t>(fx0);
    const auto x1 = static_cast<std::uint32_t>(fx1);
    const auto z0 = static_cast<std::uint32_t>(fz0);
    const auto z1 = static_cast<std::uint32_t>(fz1);
    // Capture the tiles before their first change.
    for (std::uint32_t tz = z0 / kTile; tz <= z1 / kTile; ++tz) {
        for (std::uint32_t tx = x0 / kTile; tx <= x1 / kTile; ++tx) {
            const std::uint32_t tile = tz * splat.tiles() + tx;
            if (slot_of_tile_[tile] < 0) {
                slot_of_tile_[tile] = static_cast<std::int32_t>(captured_.size());
                captured_.push_back(tile);
                before_.push_back(read_block(splat, tile, splat.maps()));
            }
        }
    }
    const float strength = std::clamp(brush_.strength, 0.0f, 1.0f) * std::clamp(dab.pressure, 0.0f, 1.0f);
    const std::uint32_t layers = splat.maps() * 4;
    for (std::uint32_t z = z0; z <= z1; ++z) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) - cx) / rt;
            const float dz = (static_cast<float>(z) - cz) / rt;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > 1.0f) {
                continue;
            }
            const float a = std::clamp(falloff_weight(brush_.falloff, d) * strength, 0.0f, 1.0f);
            if (a <= 0.0f) {
                continue;
            }
            Weights w = read_weights(splat, x, z);
            if (!erase_) {
                for (std::uint32_t j = 0; j < layers; ++j) {
                    w[j] *= 1.0f - a;
                }
                w[layer_] += a;
            } else {
                const float removed = w[layer_] * a;
                w[layer_] -= removed;
                float others = 0.0f;
                for (std::uint32_t j = 0; j < layers; ++j) {
                    others += j == layer_ ? 0.0f : w[j];
                }
                if (others > 0.0f) {
                    for (std::uint32_t j = 0; j < layers; ++j) {
                        if (j != layer_) {
                            w[j] += removed * w[j] / others;
                        }
                    }
                } else if (layer_count_ > 1) {
                    w[layer_ == 0 ? 1 : 0] += removed; // the base, or the layer above it
                } else {
                    w[layer_] += removed; // the only layer can't be erased
                }
            }
            write_weights(splat, x, z, w);
        }
    }
    for (std::uint32_t tz = z0 / kTile; tz <= z1 / kTile; ++tz) {
        for (std::uint32_t tx = x0 / kTile; tx <= x1 / kTile; ++tx) {
            splat.mark_dirty(tz * splat.tiles() + tx);
        }
    }
}

SplatDiff PaintRecorder::finish() const {
    const Splatmap& splat = *heightfield_->splat();
    SplatDiff diff;
    diff.maps = splat.maps();
    for (std::size_t i = 0; i < captured_.size(); ++i) {
        std::vector<std::uint8_t> old = before_[i];
        old.resize(block_bytes(diff.maps), 0);
        const std::vector<std::uint8_t> now = read_block(splat, captured_[i], diff.maps);
        diff.tiles.push_back({captured_[i], compress(old), compress(xor_bytes(old, now))});
    }
    return diff;
}

void PaintRecorder::restore() {
    Splatmap& splat = *heightfield_->splat();
    for (std::size_t i = 0; i < captured_.size(); ++i) {
        const auto maps = static_cast<std::uint32_t>(before_[i].size() / block_bytes(1));
        write_block(splat, captured_[i], maps, before_[i]);
    }
}

} // namespace f3d::terrain
