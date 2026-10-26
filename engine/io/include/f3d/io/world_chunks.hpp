// SPDX-License-Identifier: Apache-2.0
#pragma once

// The binary data of a scene that lives outside its components, stored as .forge
// chunks (docs/format.md): terrain heights and layer weights, painted scatter density.
//
//   terrain/<guid>/heights   "F3HF", version, samples per side, float32 metres row by row
//   terrain/<guid>/splat     "F3SP", version, tiles, maps, the RGBA8 weight maps
//   scatter/<guid>/mask      world::ScatterMask::serialize
//
// Integers are little-endian uint32. Unknown chunks are ignored on load.

#include "f3d/io/archive.hpp"

#include <map>
#include <string>

namespace f3d::scene {
class World;
}

namespace f3d::io {

/// The chunks holding `world`'s out-of-component data.
[[nodiscard]] std::map<std::string, Bytes> world_chunks(scene::World& world);

/// Restores what world_chunks() wrote into a world whose entities were just restored
/// (terrains get their heightfields back, scatters their painted density). Chunks that
/// don't match their entity (wrong size, damaged) are skipped with a warning.
void restore_world_chunks(scene::World& world, const std::map<std::string, Bytes>& chunks);

} // namespace f3d::io
