// SPDX-License-Identifier: Apache-2.0
#pragma once

// Bringing a model from outside the project into it, so it shows in the asset
// browser (which lists the project directory). UI-free and unit tested.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f3d::editor {

/// True if `path` is inside `dir` (or is it).
[[nodiscard]] bool is_inside(const std::filesystem::path& path, const std::filesystem::path& dir);

/// Copies `model` and the files it uses into `project_dir`/Imported/<stem>/, keeping
/// their layout. `dependencies` are relative to the model's directory (ModelScene's);
/// a .gltf's external buffers are found by the function itself. Returns the copy's
/// path, or nullopt with `error` set. If an identical copy is already there it is
/// reused; a different file of the same name goes to <stem>_2/, <stem>_3/, ...
/// Dependencies outside the model's directory ("../") or missing are skipped, with
/// a note in `warnings`.
[[nodiscard]] std::optional<std::filesystem::path>
copy_model_into_project(const std::filesystem::path& model, const std::vector<std::string>& dependencies,
                        const std::filesystem::path& project_dir, std::string* error,
                        std::vector<std::string>* warnings = nullptr);

} // namespace f3d::editor
