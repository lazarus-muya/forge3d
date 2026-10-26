// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/document.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/reflect_json.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"

#include <exception>
#include <system_error>

namespace f3d::editor {

Document::Document(scene::World& world, ops::OpStack& stack) : world_(world), stack_(stack) {}

bool Document::dirty() const noexcept {
    return stack_.state_id() != saved_state_;
}

std::string Document::title() const {
    std::string name = untitled() ? std::string("Untitled") : path_.filename().string();
    return dirty() ? name + "*" : name;
}

void Document::reset() {
    stack_.clear();
    world_.clear();
    path_.clear();
    saved_state_ = autosaved_state_ = stack_.state_id();
}

void Document::mark_clean() {
    saved_state_ = autosaved_state_ = stack_.state_id();
}

nlohmann::json Document::editor_state(const std::optional<scene::CameraBookmark>& camera) {
    nlohmann::json editor = nlohmann::json::object();
    if (camera) {
        editor["camera"] = core::reflect::to_json(*camera);
    }
    return editor;
}

std::optional<scene::CameraBookmark> Document::camera_from(const nlohmann::json& editor) {
    if (!editor.is_object() || !editor.contains("camera")) {
        return std::nullopt;
    }
    try {
        return core::reflect::from_json<scene::CameraBookmark>(editor.at("camera"));
    } catch (const std::exception& e) {
        F3D_LOG_WARN("Ignoring the saved editor camera: {}", e.what());
        return std::nullopt;
    }
}

io::ForgeResult Document::save(const std::filesystem::path& path, const std::optional<scene::CameraBookmark>& camera) {
    io::ForgeSaveOptions options;
    options.editor = editor_state(camera);
    io::ForgeResult result = io::save_forge(path, world_, options);
    if (!result.ok) {
        return result;
    }
    const std::filesystem::path old_autosave = autosave_target();
    path_ = path;
    saved_state_ = autosaved_state_ = stack_.state_id();
    // The autosave copy is now older than the project: drop it (and the untitled one).
    std::error_code ec;
    std::filesystem::remove(autosave_target(), ec);
    std::filesystem::remove(old_autosave, ec);
    return result;
}

io::ForgeResult Document::open(const std::filesystem::path& path, std::optional<scene::CameraBookmark>* camera) {
    stack_.clear();
    io::ForgeResult result = io::load_forge(path, world_);
    if (!result.ok) {
        reset();
        return result;
    }
    path_ = path;
    saved_state_ = autosaved_state_ = stack_.state_id();
    if (camera) {
        *camera = camera_from(result.editor);
    }
    return result;
}

io::ForgeResult Document::start_from(const std::filesystem::path& path, std::optional<scene::CameraBookmark>* camera) {
    io::ForgeResult result = open(path, camera);
    path_.clear(); // untitled either way (open() resets the scene on failure)
    saved_state_ = autosaved_state_ = stack_.state_id();
    return result;
}

io::ForgeResult Document::recover(const std::filesystem::path& project, const std::filesystem::path& autosave,
                                  std::optional<scene::CameraBookmark>* camera) {
    io::ForgeResult result = open(autosave, camera);
    if (!result.ok) {
        return result;
    }
    path_ = project;
    saved_state_ = kNeverSaved; // dirty until the user saves the recovered scene
    return result;
}

std::filesystem::path Document::autosave_target() const {
    if (untitled()) {
        return untitled_dir_.empty() ? std::filesystem::path() : untitled_dir_ / "untitled.autosave.forge";
    }
    return io::autosave_path(path_);
}

bool Document::autosave_due(double now_seconds, int minutes) const {
    return minutes > 0 && dirty() && stack_.state_id() != autosaved_state_ && !autosave_target().empty() &&
           now_seconds - last_autosave_time_ >= minutes * 60.0;
}

io::ForgeResult Document::autosave(double now_seconds, const std::optional<scene::CameraBookmark>& camera) {
    const std::filesystem::path target = autosave_target();
    if (target.empty()) {
        io::ForgeResult r;
        r.error = "no autosave location for an untitled scene";
        return r;
    }
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    io::ForgeSaveOptions options;
    options.editor = editor_state(camera);
    io::ForgeResult result = io::save_forge(target, world_, options);
    last_autosave_time_ = now_seconds;
    if (result.ok) {
        autosaved_state_ = stack_.state_id();
    }
    return result;
}

} // namespace f3d::editor
