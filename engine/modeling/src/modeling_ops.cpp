// SPDX-License-Identifier: Apache-2.0
#include "f3d/modeling/modeling_ops.hpp"

#include "f3d/core/log.hpp"
#include "f3d/modeling/editable_mesh.hpp"
#include "f3d/ops/operation.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <functional>
#include <optional>

namespace f3d::modeling {
namespace {

struct SetShapeParams {
    core::Guid entity;
    std::string shape = "cube";
    glm::vec3 size{1.0f};
    std::uint32_t segments_a = 1;
    std::uint32_t segments_b = 1;
};
struct SetMeshParams {
    core::Guid entity;
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> face_sizes;
    std::vector<std::uint32_t> corners;
};
struct FacesDistanceParams {
    core::Guid entity;
    std::vector<std::uint32_t> faces;
    float distance = 0.5f;
};
struct InsetParams {
    core::Guid entity;
    std::vector<std::uint32_t> faces;
    float amount = 0.2f;
};
struct FacesParams {
    core::Guid entity;
    std::vector<std::uint32_t> faces;
};
struct LoopCutParams {
    core::Guid entity;
    std::uint32_t a = 0; ///< the edge (position indices) the cut crosses
    std::uint32_t b = 0;
    std::uint32_t cuts = 1;
    float factor = 0.5f;
};
struct SubdivideParams {
    core::Guid entity;
    std::uint32_t levels = 1;
};
struct MirrorParams {
    core::Guid entity;
    std::int32_t axis = 0;
    float weld = 0.001f;
};
struct MergeParams {
    core::Guid entity;
    float distance = 0.001f;
};
struct OriginParams {
    core::Guid entity;
    std::string mode = "centre";
};

void register_param_types() {
    static const bool registered = [] {
        register_types();
        entt::meta_factory<SetShapeParams>{}
            .type("modeling.set_shape.params")
            .data<&SetShapeParams::entity>("entity")
            .data<&SetShapeParams::shape>("shape")
            .data<&SetShapeParams::size>("size")
            .data<&SetShapeParams::segments_a>("segments_a")
            .data<&SetShapeParams::segments_b>("segments_b");
        entt::meta_factory<SetMeshParams>{}
            .type("modeling.set_mesh.params")
            .data<&SetMeshParams::entity>("entity")
            .data<&SetMeshParams::positions>("positions")
            .data<&SetMeshParams::face_sizes>("face_sizes")
            .data<&SetMeshParams::corners>("corners");
        entt::meta_factory<FacesDistanceParams>{}
            .type("modeling.extrude.params")
            .data<&FacesDistanceParams::entity>("entity")
            .data<&FacesDistanceParams::faces>("faces")
            .data<&FacesDistanceParams::distance>("distance");
        entt::meta_factory<InsetParams>{}
            .type("modeling.inset.params")
            .data<&InsetParams::entity>("entity")
            .data<&InsetParams::faces>("faces")
            .data<&InsetParams::amount>("amount");
        entt::meta_factory<FacesParams>{}
            .type("modeling.faces.params")
            .data<&FacesParams::entity>("entity")
            .data<&FacesParams::faces>("faces");
        entt::meta_factory<LoopCutParams>{}
            .type("modeling.loop_cut.params")
            .data<&LoopCutParams::entity>("entity")
            .data<&LoopCutParams::a>("a")
            .data<&LoopCutParams::b>("b")
            .data<&LoopCutParams::cuts>("cuts")
            .data<&LoopCutParams::factor>("factor");
        entt::meta_factory<SubdivideParams>{}
            .type("modeling.subdivide.params")
            .data<&SubdivideParams::entity>("entity")
            .data<&SubdivideParams::levels>("levels");
        entt::meta_factory<MirrorParams>{}
            .type("modeling.mirror.params")
            .data<&MirrorParams::entity>("entity")
            .data<&MirrorParams::axis>("axis")
            .data<&MirrorParams::weld>("weld");
        entt::meta_factory<MergeParams>{}
            .type("modeling.merge_vertices.params")
            .data<&MergeParams::entity>("entity")
            .data<&MergeParams::distance>("distance");
        entt::meta_factory<OriginParams>{}
            .type("modeling.set_origin.params")
            .data<&OriginParams::entity>("entity")
            .data<&OriginParams::mode>("mode");
        return true;
    }();
    static_cast<void>(registered);
}

/// An edit of one entity's EditableMesh: computes the new mesh from the current one;
/// undo restores the previous mesh (and, for set_origin, the transform).
template <class P>
class MeshEditOp : public ops::OperationBase<P> {
public:
    using Edit = std::function<std::optional<EditableMesh>(const EditableMesh&, const P&, std::string&)>;
    MeshEditOp(std::string id, std::string name, Edit edit, bool keeps_shape = false, bool adds = false)
        : id_(std::move(id)), name_(std::move(name)), edit_(std::move(edit)), keeps_shape_(keeps_shape),
          adds_(adds) {}

    [[nodiscard]] std::string_view id() const override { return id_; }
    [[nodiscard]] std::string display_name() const override { return name_; }
    [[nodiscard]] std::size_t memory_size() const override {
        const auto size = [](const std::optional<EditableMesh>& m) {
            return m ? m->positions.size() * sizeof(glm::vec3) + m->corners.size() * 4 + m->face_sizes.size() * 4
                     : std::size_t{0};
        };
        return sizeof(*this) + size(before_) + size(after_);
    }

    bool execute(ops::Context& context) override {
        const entt::entity e = context.world.find(this->params.entity);
        if (e == entt::null) {
            F3D_LOG_ERROR("{}: no entity {}", id_, this->params.entity.to_string());
            return false;
        }
        auto& registry = context.world.registry();
        const EditableMesh* current = registry.try_get<EditableMesh>(e);
        if (!current && !adds_) {
            F3D_LOG_ERROR("{}: the entity has no EditableMesh", id_);
            return false;
        }
        std::string error;
        std::optional<EditableMesh> result = edit_(current ? *current : EditableMesh{}, this->params, error);
        if (!result || !is_valid(*result)) {
            F3D_LOG_ERROR("{}: {}", id_, error.empty() ? std::string("the result is not a valid mesh") : error);
            return false;
        }
        if (!keeps_shape_) {
            result->shape = Shape::None;
        }
        before_ = current ? std::optional<EditableMesh>(*current) : std::nullopt;
        after_ = std::move(*result);
        translation_before_ = context.world.transform(e).translation;
        apply_transform(context, e);
        registry.emplace_or_replace<EditableMesh>(e, *after_);
        context.changes.emit({ops::ChangeKind::ComponentChanged, this->params.entity});
        return true;
    }

    void undo(ops::Context& context) override {
        const entt::entity e = context.world.find(this->params.entity);
        if (e == entt::null) {
            return;
        }
        if (before_) {
            context.world.registry().emplace_or_replace<EditableMesh>(e, *before_);
        } else {
            context.world.registry().remove<EditableMesh>(e);
        }
        const scene::Transform& t = context.world.transform(e);
        if (t.translation != translation_before_) {
            context.world.set_local(e, translation_before_, t.rotation, t.scale);
        }
        context.changes.emit({ops::ChangeKind::ComponentChanged, this->params.entity});
    }

protected:
    /// set_origin moves the entity so the geometry stays where it was.
    virtual void apply_transform(ops::Context&, entt::entity) {}

    std::optional<EditableMesh> before_;
    std::optional<EditableMesh> after_;
    glm::vec3 translation_before_{0.0f};

private:
    std::string id_;
    std::string name_;
    Edit edit_;
    bool keeps_shape_ = false;
    bool adds_ = false;
};

class SetOriginOp final : public MeshEditOp<OriginParams> {
public:
    using MeshEditOp::MeshEditOp;

protected:
    void apply_transform(ops::Context& context, entt::entity e) override {
        // The geometry moved by `offset` in local space: move the entity by the opposite.
        const glm::vec3 offset = after_->positions.empty() || before_->positions.empty()
                                     ? glm::vec3(0.0f)
                                     : after_->positions.front() - before_->positions.front();
        const scene::Transform& t = context.world.transform(e);
        context.world.set_local(e, t.translation - t.rotation * (t.scale * offset), t.rotation, t.scale);
    }
};

template <class P>
void add(ops::OpRegistry& registry, const char* id, const char* name, typename MeshEditOp<P>::Edit edit,
         bool keeps_shape = false, bool adds = false) {
    registry.add({id, name, entt::resolve<P>()}, [=] {
        return std::make_unique<MeshEditOp<P>>(id, name, edit, keeps_shape, adds);
    });
}

} // namespace

void register_modeling_ops(ops::OpRegistry& registry) {
    register_param_types();
    add<SetShapeParams>(
        registry, "modeling.set_shape", "Set Shape",
        [](const EditableMesh& current, const SetShapeParams& p, std::string& error) -> std::optional<EditableMesh> {
            const std::optional<Shape> shape = parse_shape(p.shape);
            if (!shape || *shape == Shape::None) {
                error = "unknown shape '" + p.shape + "' (plane, cube, sphere, cylinder, cone, torus, capsule, stairs)";
                return std::nullopt;
            }
            EditableMesh mesh = make_shape({*shape, p.size, p.segments_a, p.segments_b});
            if (!current.positions.empty() || current.shape != Shape::None) {
                mesh.uv_scale = current.uv_scale; // keep the user's texture scale
            }
            return mesh;
        },
        true, true);
    add<SetMeshParams>(registry, "modeling.set_mesh", "Set Mesh",
                       [](const EditableMesh& current, const SetMeshParams& p, std::string& error) {
                           EditableMesh mesh = current;
                           mesh.positions = p.positions;
                           mesh.face_sizes = p.face_sizes;
                           mesh.corners = p.corners;
                           if (!is_valid(mesh)) {
                               error = "faces must have at least 3 corners and indices must be in range";
                               return std::optional<EditableMesh>{};
                           }
                           return std::optional<EditableMesh>{std::move(mesh)};
                       },
                       false, true);
    add<FacesDistanceParams>(registry, "modeling.extrude", "Extrude Faces",
                             [](const EditableMesh& m, const FacesDistanceParams& p, std::string& error) {
                                 if (p.faces.empty()) {
                                     error = "no faces selected";
                                     return std::optional<EditableMesh>{};
                                 }
                                 return std::optional<EditableMesh>{extrude_faces(m, p.faces, p.distance)};
                             });
    add<InsetParams>(registry, "modeling.inset", "Inset Faces",
                     [](const EditableMesh& m, const InsetParams& p, std::string& error) {
                         if (p.faces.empty()) {
                             error = "no faces selected";
                             return std::optional<EditableMesh>{};
                         }
                         return std::optional<EditableMesh>{inset_faces(m, p.faces, p.amount)};
                     });
    add<FacesParams>(registry, "modeling.delete_faces", "Delete Faces",
                     [](const EditableMesh& m, const FacesParams& p, std::string& error) {
                         if (p.faces.empty()) {
                             error = "no faces selected";
                             return std::optional<EditableMesh>{};
                         }
                         EditableMesh out = delete_faces(m, p.faces);
                         if (out.face_sizes.empty()) {
                             error = "that would delete every face (delete the object instead)";
                             return std::optional<EditableMesh>{};
                         }
                         return std::optional<EditableMesh>{std::move(out)};
                     });
    add<LoopCutParams>(registry, "modeling.loop_cut", "Loop Cut",
                       [](const EditableMesh& m, const LoopCutParams& p, std::string& error) {
                           if (p.cuts == 0 || p.cuts > 64) {
                               error = "cuts must be 1-64";
                               return std::optional<EditableMesh>{};
                           }
                           if (edge_ring(m, p.a, p.b).empty()) {
                               error = "(a, b) is not an edge of the mesh";
                               return std::optional<EditableMesh>{};
                           }
                           return std::optional<EditableMesh>{loop_cut(m, p.a, p.b, p.cuts, p.factor)};
                       });
    add<FacesParams>(registry, "modeling.flip", "Flip Normals",
                     [](const EditableMesh& m, const FacesParams& p, std::string&) {
                         return std::optional<EditableMesh>{flip_faces(m, p.faces)};
                     });
    add<SubdivideParams>(registry, "modeling.subdivide", "Subdivide",
                         [](const EditableMesh& m, const SubdivideParams& p, std::string& error) {
                             if (p.levels == 0 || p.levels > 4) {
                                 error = "levels must be 1-4";
                                 return std::optional<EditableMesh>{};
                             }
                             EditableMesh out = subdivide(m, p.levels);
                             out.smoothing_angle = 180.0f; // subdivision surfaces are smooth
                             return std::optional<EditableMesh>{std::move(out)};
                         });
    add<MirrorParams>(registry, "modeling.mirror", "Mirror",
                      [](const EditableMesh& m, const MirrorParams& p, std::string& error) {
                          if (p.axis < 0 || p.axis > 2) {
                              error = "axis must be 0 (x), 1 (y) or 2 (z)";
                              return std::optional<EditableMesh>{};
                          }
                          return std::optional<EditableMesh>{mirror(m, p.axis, std::max(p.weld, 0.0f))};
                      });
    add<MergeParams>(registry, "modeling.merge_vertices", "Merge Vertices",
                     [](const EditableMesh& m, const MergeParams& p, std::string&) {
                         return std::optional<EditableMesh>{merge_vertices(m, std::max(p.distance, 0.0f))};
                     });
    registry.add({"modeling.set_origin", "Set Origin", entt::resolve<OriginParams>()}, [] {
        return std::make_unique<SetOriginOp>(
            "modeling.set_origin", "Set Origin",
            [](const EditableMesh& m, const OriginParams& p, std::string& error) -> std::optional<EditableMesh> {
                const core::Bounds b = bounds(m);
                if (b.empty()) {
                    error = "the mesh is empty";
                    return std::nullopt;
                }
                glm::vec3 pivot = b.center();
                if (p.mode == "bottom") {
                    pivot.y = b.min.y;
                } else if (p.mode != "centre" && p.mode != "center") {
                    error = "mode must be 'centre' or 'bottom'";
                    return std::nullopt;
                }
                return translate(m, -pivot);
            },
            true);
    });
}

} // namespace f3d::modeling
