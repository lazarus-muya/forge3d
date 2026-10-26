// SPDX-License-Identifier: Apache-2.0
// editor::copy_model_into_project: imported models are brought into the project so the
// asset browser lists them.
#include <doctest/doctest.h>

#include "f3d/editor/project_import.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace f3d;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "project_import" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("project import: a glTF, its buffer and textures are copied into Imported/<stem>/") {
    const fs::path root = fresh_dir("gltf");
    const fs::path downloads = root / "downloads";
    const fs::path project = root / "project";
    fs::create_directories(project);
    write(downloads / "crate.gltf", R"({"asset":{"version":"2.0"},"buffers":[{"uri":"crate%20data.bin","byteLength":4},)"
                                    R"({"uri":"data:application/octet-stream;base64,AAAA","byteLength":3}]})");
    write(downloads / "crate data.bin", "BIN!");
    write(downloads / "textures/wood.png", "PNG");
    write(root / "shared.png", "outside");

    std::string error;
    std::vector<std::string> warnings;
    const auto copy = editor::copy_model_into_project(downloads / "crate.gltf", {"textures/wood.png", "../shared.png"},
                                                      project, &error, &warnings);
    REQUIRE_MESSAGE(copy, error);
    CHECK(*copy == project / "Imported" / "crate" / "crate.gltf");
    CHECK(read(project / "Imported/crate/crate data.bin") == "BIN!");
    CHECK(read(project / "Imported/crate/textures/wood.png") == "PNG");
    CHECK_FALSE(fs::exists(project / "Imported/shared.png"));
    CHECK(warnings.size() == 1); // ../shared.png is outside the model's folder
    CHECK(editor::is_inside(*copy, project));
    CHECK_FALSE(editor::is_inside(downloads / "crate.gltf", project));

    // The same model again reuses the copy; a different one of that name gets its own folder.
    CHECK(editor::copy_model_into_project(downloads / "crate.gltf", {}, project, &error) == *copy);
    write(root / "other/crate.gltf", R"({"asset":{"version":"2.0"}})");
    const auto other = editor::copy_model_into_project(root / "other/crate.gltf", {}, project, &error);
    REQUIRE(other);
    CHECK(*other == project / "Imported" / "crate_2" / "crate.gltf");
}

TEST_CASE("project import: a missing model fails") {
    const fs::path root = fresh_dir("missing");
    std::string error;
    CHECK_FALSE(editor::copy_model_into_project(root / "nope.glb", {}, root / "project", &error));
    CHECK_FALSE(error.empty());
}
