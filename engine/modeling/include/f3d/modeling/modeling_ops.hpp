// SPDX-License-Identifier: Apache-2.0
#pragma once

// Modeling operations (editable_mesh.hpp). Each takes the entity (an EditableMesh)
// and undoes by restoring the mesh it replaced. Faces are face indices.
//
//   modeling.set_shape      {entity, shape, size, segments_a, segments_b}
//                           (re)generates a shape (adds the EditableMesh if missing)
//   modeling.set_mesh       {entity, positions, face_sizes, corners}
//   modeling.extrude        {entity, faces, distance}
//   modeling.inset          {entity, faces, amount}
//   modeling.delete_faces   {entity, faces}
//   modeling.flip           {entity, faces}            (empty: every face)
//   modeling.subdivide      {entity, levels}
//   modeling.mirror         {entity, axis, weld}       (axis: 0 x, 1 y, 2 z)
//   modeling.merge_vertices {entity, distance}
//   modeling.set_origin     {entity, mode}             ("centre", "bottom": the geometry stays put)
//
// Every edit except set_shape turns the mesh into a plain mesh (shape None).

namespace f3d::ops {
class OpRegistry;
}

namespace f3d::modeling {

void register_modeling_ops(ops::OpRegistry& registry);

} // namespace f3d::modeling
