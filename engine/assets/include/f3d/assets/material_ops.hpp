// SPDX-License-Identifier: Apache-2.0
#pragma once

// Material operations (phases.md P4.2). Materials are project files (.f3dmat), so
// these operations write files through ops::Context::files and restore the
// previous contents on undo. They emit ChangeKind::AssetChanged with the path.
//
//   material.create        {path, material}            writes a new .f3dmat (fails if it exists)
//   material.set_property  {path, property, value}     sets one field (consecutive edits merge)
//
// Assigning a material to a mesh is entity.set_property on "MeshRenderer.materials".

#include "f3d/ops/registry.hpp"

namespace f3d::assets {

void register_material_ops(ops::OpRegistry& registry);

} // namespace f3d::assets
