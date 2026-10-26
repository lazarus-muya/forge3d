// SPDX-License-Identifier: Apache-2.0
#pragma once

// Polygon modeling: an EditableMesh component holds a polygon mesh (faces of any size,
// counter-clockwise seen from outside) saved with the scene. Shapes (plane, cube,
// sphere, ...) start one; the modeling.* operations (modeling_ops.hpp) edit it:
// extrude, inset, delete and flip faces, subdivide, mirror, merge vertices, set origin.
//
// For drawing, sync_meshes() triangulates every changed EditableMesh into the mesh
// library under "modeling://<entity guid>" and points the entity's MeshRenderer at it,
// so materials, shadows, picking and framing work as for imported meshes.

#include "f3d/assets/mesh.hpp"
#include "f3d/core/math.hpp"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::assets {
class MeshLibrary;
}

namespace f3d::modeling {

/// The shape a mesh was created as. While it is unedited (shape != None) its
/// parameters can still change (modeling.set_shape regenerates it); any other edit
/// turns it into a plain mesh (None).
enum class Shape : std::uint8_t { None, Plane, Cube, Sphere, Cylinder, Cone, Torus, Capsule, Stairs };

[[nodiscard]] std::string_view to_string(Shape shape) noexcept;
[[nodiscard]] std::optional<Shape> parse_shape(std::string_view text) noexcept;

/// Shape parameters. `size` is the bounding box (metres); the segment counts mean, by shape:
/// plane: x and z subdivisions; cube: subdivisions per edge (a); sphere: around (a), rings (b);
/// cylinder/cone: sides (a), height segments (b); torus: around the ring (a), around the tube (b);
/// capsule: sides (a), rings per cap (b); stairs: steps (a). size: torus x = outer diameter,
/// y = tube diameter; stairs x = width, y = total height, z = run.
struct ShapeParams {
    Shape shape = Shape::Cube;
    glm::vec3 size{1.0f};
    std::uint32_t segments_a = 1;
    std::uint32_t segments_b = 1;

    friend bool operator==(const ShapeParams&, const ShapeParams&) = default;
};

/// Default parameters of a shape (a 1 m cube, a 1 m sphere with 32 x 16 segments, ...).
[[nodiscard]] ShapeParams default_shape(Shape shape) noexcept;

/// A polygon mesh in the entity's local space.
struct EditableMesh {
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> face_sizes; ///< corners per face (>= 3)
    std::vector<std::uint32_t> corners;    ///< position index per corner, faces back to back
    float smoothing_angle = 30.0f;         ///< degrees: edges sharper than this shade hard (0: flat)
    float uv_scale = 1.0f;                 ///< metres per texture repeat (box projection)
    Shape shape = Shape::None;             ///< the unedited shape, or None
    glm::vec3 shape_size{1.0f};
    std::uint32_t shape_segments_a = 1;
    std::uint32_t shape_segments_b = 1;

    friend bool operator==(const EditableMesh&, const EditableMesh&) = default;

    [[nodiscard]] std::size_t face_count() const noexcept { return face_sizes.size(); }
    [[nodiscard]] ShapeParams shape_params() const noexcept {
        return {shape, shape_size, shape_segments_a, shape_segments_b};
    }
};

/// Offsets of each face's first corner (prefix sums of face_sizes).
[[nodiscard]] std::vector<std::uint32_t> face_offsets(const EditableMesh& mesh);

/// True if every face has >= 3 corners, the corners add up and every index is in range.
[[nodiscard]] bool is_valid(const EditableMesh& mesh) noexcept;

/// The geometry of a shape (shape parameters recorded, smoothing chosen for it).
[[nodiscard]] EditableMesh make_shape(const ShapeParams& params);

/// Unit normal of a face (Newell's method; zero for a degenerate face).
[[nodiscard]] glm::vec3 face_normal(const EditableMesh& mesh, std::uint32_t face);
[[nodiscard]] glm::vec3 face_centre(const EditableMesh& mesh, std::uint32_t face);

/// Triangulated for drawing: one vertex per corner, normals smoothed across edges
/// flatter than the smoothing angle, box-projected UVs, white vertex colour.
[[nodiscard]] assets::MeshData triangulate(const EditableMesh& mesh, std::string_view name = {});

/// The face hit by a ray (local space) and the distance along it, if any.
struct FaceHit {
    std::uint32_t face = 0;
    float distance = 0.0f;
};
[[nodiscard]] std::optional<FaceHit> raycast(const EditableMesh& mesh, const glm::vec3& origin,
                                             const glm::vec3& direction);

// --- Editing (pure functions: the operations wrap them) -------------------------------------

/// Extrudes the faces (connected faces as one region) by `distance` along their averaged
/// normals. The extruded faces keep their indices; the side faces are appended.
[[nodiscard]] EditableMesh extrude_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces,
                                         float distance);
/// Insets each face: a smaller copy, `amount` (0-1) of the way towards its centre, joined
/// to the original outline by a ring of quads. The inner faces keep the indices.
[[nodiscard]] EditableMesh inset_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces,
                                       float amount);
/// The edge ring through the edge (a, b) (position indices): the edges across the quads
/// on either side of it, each oriented like (a, b) (first = the end on a's side), starting
/// with (a, b). Stops at faces that are not quads and at open edges; empty if (a, b) is
/// not an edge of the mesh.
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> edge_ring(const EditableMesh& mesh,
                                                                              std::uint32_t a, std::uint32_t b);
/// Loop cut: splits the quads of the edge ring through (a, b) with `cuts` new edge loops.
/// One cut sits `factor` (0-1) of the way from a's side; more are spaced evenly. Faces
/// next to the ring that are not split get the new positions in their outline (no
/// T-junctions). Returns the mesh unchanged if (a, b) is not an edge.
[[nodiscard]] EditableMesh loop_cut(const EditableMesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t cuts,
                                    float factor = 0.5f);
/// Removes the faces, and the positions no remaining face uses.
[[nodiscard]] EditableMesh delete_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces);
/// Reverses the winding (the normals) of the faces; every face if `faces` is empty.
[[nodiscard]] EditableMesh flip_faces(const EditableMesh& mesh, const std::vector<std::uint32_t>& faces);
/// Catmull-Clark subdivision, `levels` times (open edges stay put as creases' curves).
[[nodiscard]] EditableMesh subdivide(const EditableMesh& mesh, std::uint32_t levels);
/// Adds a mirrored copy across the local plane through the origin perpendicular to
/// `axis` (0 x, 1 y, 2 z), welding positions within `weld` of the plane.
[[nodiscard]] EditableMesh mirror(const EditableMesh& mesh, int axis, float weld);
/// Merges positions closer than `distance`; drops faces that collapse.
[[nodiscard]] EditableMesh merge_vertices(const EditableMesh& mesh, float distance);
/// Moves every position by `offset` (the origin moves by -offset relative to the geometry).
[[nodiscard]] EditableMesh translate(const EditableMesh& mesh, const glm::vec3& offset);
/// Local-space bounds of the positions.
[[nodiscard]] core::Bounds bounds(const EditableMesh& mesh);

/// A polygon mesh from triangles (MeshData), with coincident positions welded and
/// coplanar triangle pairs sharing an edge merged into quads.
[[nodiscard]] EditableMesh from_triangles(const assets::MeshData& mesh);

/// Registers EditableMesh and its value types with reflection (idempotent).
void register_types();

/// The mesh-library path of an entity's triangulated mesh: "modeling://<guid>".
[[nodiscard]] std::string mesh_path(scene::World& world, entt::entity entity);

/// Triangulates EditableMeshes that changed since the last call into `meshes`, and
/// points their MeshRenderers (added if missing) at them. Cheap when nothing changed.
void sync_meshes(scene::World& world, assets::MeshLibrary& meshes);

} // namespace f3d::modeling
