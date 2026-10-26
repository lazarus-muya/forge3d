// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/builtin.hpp"

#include "f3d/scene/layers.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <memory>
#include <optional>
#include <string>

namespace f3d::ops {
namespace {

using scene::CameraBookmark;
using scene::LayerTable;

struct BookmarkNameParams {
    std::string name;
};

void register_param_types() {
    scene::register_types(); // bookmark.save's params are a scene::CameraBookmark
    entt::meta_factory<BookmarkNameParams>{}.type("bookmark.name.params").data<&BookmarkNameParams::name>("name");
}

void bookmarks_changed(Context& context) {
    context.changes.emit({ChangeKind::BookmarksChanged, {}});
}

class BookmarkSaveOp final : public OperationBase<CameraBookmark> {
public:
    [[nodiscard]] std::string_view id() const override { return "bookmark.save"; }
    [[nodiscard]] std::string display_name() const override { return "Save bookmark " + params.name; }

    bool execute(Context& context) override {
        if (params.name.empty()) {
            return false;
        }
        scene::BookmarkTable& table = context.world.bookmarks();
        if (const CameraBookmark* existing = table.find(params.name)) {
            previous_ = *existing; // replaced: undo restores it
        } else {
            previous_.reset();
        }
        table.set(params);
        bookmarks_changed(context);
        return true;
    }

    void undo(Context& context) override {
        if (previous_) {
            context.world.bookmarks().set(*previous_);
        } else {
            context.world.bookmarks().remove(params.name);
        }
        bookmarks_changed(context);
    }

private:
    std::optional<CameraBookmark> previous_;
};

class BookmarkDeleteOp final : public OperationBase<BookmarkNameParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "bookmark.delete"; }
    [[nodiscard]] std::string display_name() const override { return "Delete bookmark " + params.name; }

    bool execute(Context& context) override {
        scene::BookmarkTable& table = context.world.bookmarks();
        const CameraBookmark* existing = table.find(params.name);
        if (!existing) {
            return false;
        }
        deleted_ = *existing;
        index_ = table.index_of(params.name);
        table.remove(params.name);
        bookmarks_changed(context);
        return true;
    }

    void undo(Context& context) override {
        context.world.bookmarks().set(deleted_, index_);
        bookmarks_changed(context);
    }

private:
    CameraBookmark deleted_;
    std::size_t index_ = LayerTable::npos;
};

/// Moves the editor camera to a bookmark. It changes no saved state, so it is
/// transient: not recorded in the history.
class BookmarkRecallOp final : public OperationBase<BookmarkNameParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "bookmark.recall"; }
    [[nodiscard]] std::string display_name() const override { return "Recall bookmark " + params.name; }
    [[nodiscard]] bool records_history() const override { return false; }

    bool execute(Context& context) override {
        const CameraBookmark* bookmark = context.world.bookmarks().find(params.name);
        if (!bookmark) {
            return false;
        }
        context.world.view_request() = *bookmark;
        context.changes.emit({ChangeKind::ViewRequested, {}});
        return true;
    }

    void undo(Context&) override {}
};

} // namespace

void register_bookmark_ops(OpRegistry& registry) {
    register_param_types();
    registry.add({"bookmark.save", "Save Camera Bookmark", entt::resolve<CameraBookmark>()},
                 [] { return std::make_unique<BookmarkSaveOp>(); });
    registry.add({"bookmark.delete", "Delete Camera Bookmark", entt::resolve<BookmarkNameParams>()},
                 [] { return std::make_unique<BookmarkDeleteOp>(); });
    registry.add({"bookmark.recall", "Recall Camera Bookmark", entt::resolve<BookmarkNameParams>()},
                 [] { return std::make_unique<BookmarkRecallOp>(); });
}

} // namespace f3d::ops
