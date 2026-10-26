// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/selection_ops.hpp"

#include "f3d/scene/selection.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <memory>
#include <string>
#include <vector>

namespace f3d::ops {
namespace {

using core::Guid;

struct SelectionSetParams {
    std::vector<Guid> entities; ///< in order; the first is the primary selection
    Guid active;                ///< null: the first entity
};

struct SelectionEntityParams {
    Guid entity;
};

void register_param_types() {
    core::reflect::register_value_types();
    entt::meta_factory<SelectionSetParams>{}
        .type("selection.set.params")
        .data<&SelectionSetParams::entities>("entities")
        .data<&SelectionSetParams::active>("active");
    entt::meta_factory<SelectionEntityParams>{}
        .type("selection.entity.params")
        .data<&SelectionEntityParams::entity>("entity");
}

/// Base for the selection operations: records the previous selection for undo.
template <class P>
class SelectionOp : public OperationBase<P> {
public:
    bool execute(Context& context) override {
        scene::Selection& selection = context.world.selection();
        if (!has_before_) {
            before_ = selection;
            has_before_ = true;
        }
        if (!apply(context.world, selection)) {
            return false;
        }
        context.changes.emit({ChangeKind::SelectionChanged, selection.active()});
        return true;
    }

    void undo(Context& context) override {
        if (has_before_) {
            context.world.selection() = before_;
        }
        context.changes.emit({ChangeKind::SelectionChanged, context.world.selection().active()});
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (before_.size() + 4) * sizeof(Guid);
    }

protected:
    virtual bool apply(const scene::World& world, scene::Selection& selection) = 0;

    scene::Selection before_;
    bool has_before_ = false;
};

class SelectionSetOp final : public SelectionOp<SelectionSetParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "selection.set"; }
    [[nodiscard]] std::string display_name() const override {
        return params.entities.empty() ? "Deselect all" : "Select";
    }

protected:
    bool apply(const scene::World& world, scene::Selection& selection) override {
        // Unknown entities are dropped (an RPC client may race a delete).
        std::vector<Guid> entities;
        entities.reserve(params.entities.size());
        for (const Guid& guid : params.entities) {
            if (world.find(guid) != entt::null) {
                entities.push_back(guid);
            }
        }
        const Guid active = world.find(params.active) != entt::null ? params.active : Guid{};
        selection.set(std::move(entities), active);
        return true;
    }
};

class SelectionAddOp final : public SelectionOp<SelectionEntityParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "selection.add"; }
    [[nodiscard]] std::string display_name() const override { return "Add to selection"; }

protected:
    bool apply(const scene::World& world, scene::Selection& selection) override {
        if (world.find(params.entity) == entt::null) {
            return false;
        }
        selection.add(params.entity);
        selection.set_active(params.entity);
        return true;
    }
};

class SelectionRemoveOp final : public SelectionOp<SelectionEntityParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "selection.remove"; }
    [[nodiscard]] std::string display_name() const override { return "Remove from selection"; }

protected:
    bool apply(const scene::World&, scene::Selection& selection) override {
        if (!selection.contains(params.entity)) {
            return false;
        }
        selection.remove(params.entity);
        return true;
    }
};

} // namespace

void register_selection_ops(OpRegistry& registry) {
    register_param_types();
    registry.add({"selection.set", "Select", entt::resolve<SelectionSetParams>()},
                 [] { return std::make_unique<SelectionSetOp>(); });
    registry.add({"selection.add", "Add to Selection", entt::resolve<SelectionEntityParams>()},
                 [] { return std::make_unique<SelectionAddOp>(); });
    registry.add({"selection.remove", "Remove from Selection", entt::resolve<SelectionEntityParams>()},
                 [] { return std::make_unique<SelectionRemoveOp>(); });
}

} // namespace f3d::ops
