// SPDX-License-Identifier: Apache-2.0
#pragma once

// The open project (phases.md P2.6): its .forge path, whether it has unsaved
// changes, saving/opening, autosave and crash recovery. UI-free and unit tested;
// the App adds the dialogs and prompts.

#include "f3d/io/forge_file.hpp"
#include "f3d/scene/layers.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::ops {
class OpStack;
} // namespace f3d::ops

namespace f3d::editor {

class Document {
public:
    Document(scene::World& world, ops::OpStack& stack);

    /// The project file; empty while untitled.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] bool untitled() const noexcept { return path_.empty(); }
    /// True if the scene changed since it was last saved or opened.
    [[nodiscard]] bool dirty() const noexcept;
    /// "name.forge" or "Untitled", with a trailing '*' when dirty.
    [[nodiscard]] std::string title() const;

    /// An empty, untitled, clean scene with no history.
    void reset();
    /// Marks the current state as saved (e.g. after importing a model into a new scene).
    void mark_clean();

    /// Saves to `path` (the document's path becomes `path`). `camera` is stored as
    /// editor state. On success the document is clean and the old autosave is removed.
    io::ForgeResult save(const std::filesystem::path& path, const std::optional<scene::CameraBookmark>& camera = {});
    /// Opens `path`, replacing the scene and clearing the history. On success the
    /// document is clean; `camera` receives the saved editor camera, if any.
    io::ForgeResult open(const std::filesystem::path& path, std::optional<scene::CameraBookmark>* camera = nullptr);
    /// Starts a new scene from a template (the startup scene): loads `path` but stays
    /// untitled and clean, so saving asks for a new file and the template is never written.
    io::ForgeResult start_from(const std::filesystem::path& path,
                               std::optional<scene::CameraBookmark>* camera = nullptr);
    /// Loads `autosave` (a newer copy of `project`) in place of `project`: the
    /// document's path is `project` and it is dirty, so the user saves it deliberately.
    io::ForgeResult recover(const std::filesystem::path& project, const std::filesystem::path& autosave,
                            std::optional<scene::CameraBookmark>* camera = nullptr);

    // --- Autosave -----------------------------------------------------------------------------

    /// Where autosaves go: "<project>.autosave.forge", or "<untitled_dir>/untitled.autosave.forge".
    [[nodiscard]] std::filesystem::path autosave_target() const;
    void set_untitled_autosave_dir(std::filesystem::path dir) { untitled_dir_ = std::move(dir); }
    /// True if autosave is on (`minutes` > 0), there are changes not yet autosaved,
    /// and `minutes` have passed since the last autosave (or since the last save/open).
    [[nodiscard]] bool autosave_due(double now_seconds, int minutes) const;
    /// Writes the autosave copy; the document stays dirty.
    io::ForgeResult autosave(double now_seconds, const std::optional<scene::CameraBookmark>& camera = {});
    /// Restarts the autosave interval (call when a document is opened or saved).
    void restart_autosave_clock(double now_seconds) noexcept { last_autosave_time_ = now_seconds; }

    /// Editor state stored in editor.json.
    [[nodiscard]] static nlohmann::json editor_state(const std::optional<scene::CameraBookmark>& camera);
    [[nodiscard]] static std::optional<scene::CameraBookmark> camera_from(const nlohmann::json& editor);

private:
    static constexpr std::uint64_t kNeverSaved = ~std::uint64_t{0};

    scene::World& world_;
    ops::OpStack& stack_;
    std::filesystem::path path_;
    std::filesystem::path untitled_dir_;
    std::uint64_t saved_state_ = 0;     ///< OpStack::state_id() when saved / opened
    std::uint64_t autosaved_state_ = 0; ///< OpStack::state_id() at the last autosave
    double last_autosave_time_ = 0.0;
};

} // namespace f3d::editor
