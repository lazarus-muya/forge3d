// SPDX-License-Identifier: Apache-2.0
#pragma once

// The operation model (design §3.B, phases.md P2.3). Every scene mutation is an
// Operation with reflected parameters, execute/undo, optional merge (so a drag
// collapses into one undo step) and a memory size for the capped history.

#include "f3d/core/guid.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/core/reflect_json.hpp"

#include <entt/meta/meta.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::core::vfs {
class FileSystem;
} // namespace f3d::core::vfs

namespace f3d::ops {

/// A committed scene change. Subscribers (UI, renderer, live link) react to it.
enum class ChangeKind : std::uint8_t {
    EntityCreated,
    EntityDestroyed,
    ComponentChanged,
    HierarchyChanged,
    SelectionChanged,
    LayersChanged,    ///< the layer table changed (entity: null)
    BookmarksChanged, ///< the bookmark table changed (entity: null)
    ViewRequested,    ///< bookmark.recall set World::view_request() for the editor camera
    AssetChanged,     ///< an operation rewrote an asset file (`asset`: its VFS path; entity: null)
};

struct ChangeEvent {
    ChangeKind kind = ChangeKind::ComponentChanged;
    core::Guid entity;
    std::string asset; ///< AssetChanged: the asset's VFS path
};

/// Fan-out of change events to subscribers.
class ChangeBus {
public:
    using Token = std::uint64_t;
    using Callback = std::function<void(const ChangeEvent&)>;

    Token subscribe(Callback callback);
    void unsubscribe(Token token);
    void emit(const ChangeEvent& event) const;

private:
    std::unordered_map<Token, Callback> subscribers_;
    Token next_ = 1;
};

/// Everything an operation needs to run: the scene and the change bus.
struct Context {
    Context(scene::World& world_, ChangeBus& changes_, core::vfs::FileSystem* files_ = nullptr)
        : world(world_), changes(changes_), files(files_) {}

    scene::World& world;
    ChangeBus& changes;
    /// Project files, for operations that read or write assets (prefabs). May be
    /// null (tests, tools without a project); such operations then fail.
    core::vfs::FileSystem* files = nullptr;
    /// Long operations (terrain.erode) report their progress (0..1) here; returning
    /// false cancels them (they then fail and change nothing). Optional.
    std::function<bool(float)> progress;
};

/// One undoable scene mutation.
class Operation {
public:
    virtual ~Operation() = default;

    [[nodiscard]] virtual std::string_view id() const = 0;
    [[nodiscard]] virtual std::string display_name() const = 0;
    /// Reflected parameter type (drives JSON params and the RPC/MCP schema).
    [[nodiscard]] virtual entt::meta_type params_type() const = 0;
    [[nodiscard]] virtual nlohmann::json params_json() const = 0;
    virtual void set_params_json(const nlohmann::json& params) = 0;

    /// Applies the operation. Returns false (and leaves the scene unchanged) on
    /// invalid parameters. Must record enough state for undo().
    virtual bool execute(Context& context) = 0;
    /// Reverses execute(). Only called after a successful execute().
    virtual void undo(Context& context) = 0;

    /// Folds a consecutive edit into this one (a drag). Returns true if merged;
    /// `next` is then discarded and this operation's undo must still restore the
    /// state from before the first edit.
    virtual bool merge(const Operation& next) {
        (void)next;
        return false;
    }

    /// Approximate heap size, counted against the history's byte cap.
    [[nodiscard]] virtual std::size_t memory_size() const { return sizeof(*this); }

    /// False for transient operations that change no saved scene state (e.g.
    /// bookmark.recall moves the editor camera): they run but are not recorded,
    /// so they neither create an undo step nor clear the redo history.
    [[nodiscard]] virtual bool records_history() const { return true; }
};

/// Convenience base for operations with a reflected parameter struct `P`.
template <class P>
class OperationBase : public Operation {
public:
    P params;

    [[nodiscard]] entt::meta_type params_type() const override { return entt::resolve<P>(); }
    [[nodiscard]] nlohmann::json params_json() const override { return core::reflect::to_json(params); }
    void set_params_json(const nlohmann::json& json) override { params = core::reflect::from_json<P>(json); }
};

} // namespace f3d::ops
