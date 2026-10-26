// SPDX-License-Identifier: Apache-2.0
// forge3d-cli: headless project editing (phases.md P2.6).
//
// Commands run in order on one session (a scene plus its undo history), so one
// invocation can open, edit, undo/redo and save:
//
//   forge3d-cli open in.forge run-op entity.create --params '{"name":"Rock"}' save out.forge
//
// The first failing command stops the run with exit code 1.
#include "f3d/api/host.hpp"
#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/material_ops.hpp"
#include "f3d/assets/mesh_library.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/core/image.hpp"
#include "f3d/core/image_compare.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/system.hpp"
#include "f3d/core/version.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/io/forge_file.hpp"
#include "f3d/modeling/modeling_ops.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/script.hpp"
#include "f3d/ops/stack.hpp"
#if defined(F3D_WITH_PYTHON)
#include "f3d/python/python.hpp"
#endif
#include "f3d/render/raster/capture.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain_ops.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/spline.hpp"
#include "f3d/world/world_ops.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace f3d;

constexpr std::string_view kUsage =
    R"(Usage: forge3d-cli [--quiet] [--rhi d3d12|vulkan] [--adapter <choice>] <command> ...

Options (before the commands):
  --quiet                     no progress output
  --plugins <dir>             load plugins from <dir> (default: plugins/ next to forge3d-cli)
  --no-plugins                don't load plugins
  --rhi d3d12|vulkan          the GPU backend for rendering commands
  --adapter <choice>          an adapter index, discrete, integrated or software
                              (WARP / lavapipe: the golden-image tests)

Commands run in order on one scene and its undo history:
  new                         start an empty scene (clears the history)
  project <dir>               mount <dir> as project:// (prefabs and other assets)
  open <file.forge>           load a project (runs format migrations)
  save <file.forge>           save the scene atomically
  run-op <id> [--params <json>]
                              run an operation, e.g. run-op entity.create --params {"name":"Rock"}
                              (viewport.capture renders headless: {"path":"out.png","width":1280,...})
  run <ops.json>              run a forge3d.ops script (one undo step)
  run <script.py>             run a Python script with the forge3d module (-full builds)
  undo | redo                 step through the history
  list-ops [--json]           list the operations and their parameters
  print [--out <file>]        write the scene as JSON (stdout, or a file)
  info                        summary of the scene
  import <dir> [--force]      scan a project directory and import (cook) its stale assets;
                              --force re-imports everything
  compare <a.png> <b.png> [--ssim <min>]
                              fail unless the images match (mean SSIM, default 0.99)
  golden <spec.json> [--update] [--out <dir>]
                              render a golden-image test and compare it with its reference
                              (SSIM); --update writes the reference instead
  version                     print the forge3d version
  help                        show this help
)";

struct Session {
    core::vfs::FileSystem files;
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes, &files};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    bool quiet = false;

    // viewport.capture renders headless with a device created on first use.
    render::rhi::DeviceDesc device_desc;
    assets::MeshLibrary meshes;
    // Plugins (phases.md P5.1) register their operations into this session.
    api::Host host{registry, stack, world};
    api::PluginLoader plugins{host};
#if defined(F3D_WITH_PYTHON)
    std::unique_ptr<python::Interpreter> python; ///< started by the first `run <script.py>`
#endif
    std::unique_ptr<render::rhi::Device> device;
    std::unique_ptr<render::rhi::ShaderLibrary> shaders;
    std::unique_ptr<render::raster::Capturer> capturer;

    Session() {
        ops::register_builtin_ops(registry);
        assets::register_material_ops(registry);
        terrain::register_terrain_ops(registry);
        world::register_world_ops(registry); // scatter painting, water, splines
        for (auto& [path, mesh] : world::builtin_scatter_meshes()) {
            meshes.add(path, std::move(mesh)); // what scatter presets and fences place
        }
        for (auto& [path, mesh] : world::builtin_spline_meshes()) {
            meshes.add(path, std::move(mesh));
        }
        modeling::register_modeling_ops(registry); // shapes and mesh edits
        render::raster::register_capture_op(registry,
                                            [this](scene::World& w, const render::raster::CaptureRequest& request,
                                                   std::string* error) { return capture(w, request, error); });
        stack.set_limits(100000, 0); // a CLI run keeps its whole history
    }

    int run_python(const std::string& path) {
#if defined(F3D_WITH_PYTHON)
        if (!python) {
            api::RpcServices services;
            services.host = &host;
            services.changes = &changes;
            services.capture = [this](scene::World& w, const render::raster::CaptureRequest& request,
                                      std::string* error) { return capture(w, request, error); };
            python = std::make_unique<python::Interpreter>(std::move(services));
            std::string error;
            if (!python->start(&error)) {
                std::cerr << "forge3d-cli: " << error << '\n';
                return 1;
            }
        }
        const python::RunResult r = python->run_file(path, false);
        if (!r.ok) {
            std::cerr << r.error;
            std::cerr << "forge3d-cli: run " << path << " failed\n";
            return 1;
        }
        say("ran {}", path);
        return 0;
#else
        std::cerr << "forge3d-cli: run " << path
                  << ": this build has no Python (configure with FORGE3D_WITH_PYTHON=ON, e.g. the -full presets)\n";
        return 1;
#endif
    }

    bool capture(scene::World& w, const render::raster::CaptureRequest& request, std::string* error) {
        if (!capturer) {
            auto created = render::rhi::Device::create(device_desc);
            if (!created.device) {
                *error = "no GPU device: " + created.error;
                return false;
            }
            device = std::move(created.device);
            say("rendering with {} on '{}'", render::rhi::to_string(device->backend()), device->adapter().name);
            shaders = std::make_unique<render::rhi::ShaderLibrary>(*device, render::rhi::default_shader_library_desc());
            capturer = std::make_unique<render::raster::Capturer>(*device, *shaders);
        }
        return capturer->capture(w, meshes, request, nullptr, error);
    }

    template <class... Args>
    void say(fmt::format_string<Args...> format, Args&&... args) const {
        if (!quiet) {
            std::cout << fmt::format(format, std::forward<Args>(args)...) << '\n';
        }
    }
};

int fail(const std::string& message) {
    std::cerr << "forge3d-cli: " << message << '\n';
    return 1;
}

void list_ops(const Session& s, bool as_json) {
    nlohmann::json out = nlohmann::json::array();
    for (const ops::OpInfo& info : s.registry.list()) {
        const nlohmann::json schema = ops::param_schema(info);
        if (as_json) {
            out.push_back({{"id", info.id}, {"name", info.display_name}, {"params", schema}});
            continue;
        }
        std::string params;
        for (const auto& field : schema) {
            params += (params.empty() ? "" : ", ") + field.at("name").get<std::string>() + ": " +
                      field.at("type").get<std::string>();
        }
        std::cout << info.id << "  (" << info.display_name << ")\n    " << (params.empty() ? "-" : params) << '\n';
    }
    if (as_json) {
        std::cout << out.dump(2) << '\n';
    }
}

/// A golden-image test (phases.md P4.5): build a scene from scripts and ops, capture
/// it, compare with the reference (SSIM). See tests/golden/README.md.
int golden(Session& s, const std::filesystem::path& spec_path, bool update, const std::filesystem::path& out_dir) {
    nlohmann::json spec;
    try {
        std::ifstream file(spec_path);
        if (!file) {
            return fail("golden: cannot read " + spec_path.string());
        }
        spec = nlohmann::json::parse(file);
    } catch (const std::exception& e) {
        return fail("golden: " + spec_path.string() + ": " + e.what());
    }
    if (spec.value("format", "") != "forge3d.golden") {
        return fail("golden: " + spec_path.string() + " is not a forge3d.golden spec");
    }
    const std::string name = spec_path.stem().string();
    s.stack.clear();
    s.world.clear();
    for (const auto& script : spec.value("scripts", nlohmann::json::array())) {
        const ops::ScriptResult r = ops::run_script_file(s.stack, s.registry, script.get<std::string>());
        if (!r.ok) {
            return fail("golden " + name + ": " + script.get<std::string>() + ": " + r.error);
        }
    }
    for (const auto& model : spec.value("models", nlohmann::json::array())) {
        const std::filesystem::path path = model.get<std::string>();
        std::string error;
        const auto gltf = s.meshes.load_file(path, &error);
        if (!gltf) {
            return fail("golden " + name + ": " + path.string() + ": " + error);
        }
        core::Guid root;
        const ops::ScriptResult r =
            ops::run_script(s.stack, s.registry, assets::gltf_import_ops(*gltf, path, {}, root));
        if (!r.ok) {
            return fail("golden " + name + ": importing " + path.string() + ": " + r.error);
        }
    }
    if (spec.contains("ops")) {
        nlohmann::json script = {{"format", "forge3d.ops"}, {"version", 1}, {"ops", spec.at("ops")}};
        const ops::ScriptResult r = ops::run_script(s.stack, s.registry, script);
        if (!r.ok) {
            return fail("golden " + name + ": " + r.error);
        }
    }

    const nlohmann::json capture = spec.value("capture", nlohmann::json::object());
    render::raster::CaptureRequest request;
    request.width = capture.value("width", 480u);
    request.height = capture.value("height", 270u);
    request.bookmark = capture.value("bookmark", "");
    const auto mode = render::raster::parse_view_mode(capture.value("mode", "lit"));
    if (!mode) {
        return fail("golden " + name + ": unknown capture mode");
    }
    request.mode = *mode;
    request.screen_percentage = capture.value("screen_percentage", 100.0f);
    const auto fog_quality = render::raster::parse_fog_quality(capture.value("fog_quality", "high"));
    if (!fog_quality) {
        return fail("golden " + name + ": unknown fog_quality");
    }
    request.fog_quality = *fog_quality;
    std::filesystem::create_directories(out_dir);
    const std::filesystem::path actual = out_dir / (name + ".png");
    request.path = actual.string();
    std::string error;
    if (!s.capture(s.world, request, &error)) {
        return fail("golden " + name + ": " + error);
    }

    // The reference: a backend-specific one (name.vulkan.png) wins over the shared one.
    const std::filesystem::path dir = spec_path.parent_path();
    const std::string reference_name = spec.value("reference", name + ".png");
    const std::string backend(render::rhi::to_string(s.device->backend()));
    std::filesystem::path reference = dir / reference_name;
    const std::filesystem::path specific =
        dir / (std::filesystem::path(reference_name).stem().string() + "." + backend + ".png");
    if (std::filesystem::exists(specific)) {
        reference = specific;
    }
    if (update) {
        std::filesystem::copy_file(actual, reference, std::filesystem::copy_options::overwrite_existing);
        s.say("golden {}: reference written ({})", name, reference.string());
        return 0;
    }
    const auto expected = core::read_png(reference);
    if (!expected) {
        return fail("golden " + name + ": no reference " + reference.string() + " (run with --update)");
    }
    const auto image = core::read_png(actual);
    const core::ImageComparison result = core::compare_images(*image, *expected);
    const nlohmann::json threshold = spec.value("threshold", nlohmann::json::object());
    const double min_ssim = threshold.value("ssim", 0.99);
    const double min_block = threshold.value("block_ssim", 0.90);
    const bool pass = result.same_size && result.ssim >= min_ssim && result.min_ssim >= min_block;
    const std::string summary = fmt::format(
        "golden {}: SSIM {:.5f} (min {:.3f}), worst block {:.4f} (min {:.3f}), mean diff {:.3f}, max diff {}, "
        "{} pixels differ",
        name, result.ssim, min_ssim, result.min_ssim, min_block, result.mean_difference, result.max_difference,
        result.differing_pixels);
    if (!pass) {
        const std::filesystem::path diff = out_dir / (name + ".diff.png");
        if (result.same_size) {
            core::write_png(diff, core::difference_image(*image, *expected));
        }
        return fail(summary + (result.same_size ? "; actual " + actual.string() + ", diff " + diff.string()
                                                : "; size differs from the reference"));
    }
    s.say("{}", summary);
    return 0;
}

int run(std::vector<std::string_view> args) {
    Session s;
    std::size_t i = 0;
    std::optional<std::filesystem::path> plugin_dir = api::default_plugin_dir();
    for (; i < args.size(); ++i) {
        if (args[i] == "--quiet") {
            s.quiet = true;
        } else if (args[i] == "--plugins" && i + 1 < args.size()) {
            plugin_dir = std::filesystem::path(args[++i]);
        } else if (args[i] == "--no-plugins") {
            plugin_dir.reset();
        } else if (args[i] == "--rhi" && i + 1 < args.size()) {
            const auto backend = render::rhi::parse_backend(args[++i]);
            if (!backend) {
                return fail("--rhi expects d3d12 or vulkan");
            }
            s.device_desc.backend = *backend;
            s.device_desc.allow_fallback = false;
        } else if (args[i] == "--adapter" && i + 1 < args.size()) {
            if (!render::rhi::apply_adapter_choice(args[++i], s.device_desc)) {
                return fail("--adapter expects an index, discrete, integrated or software");
            }
        } else {
            break;
        }
    }
    // Long operations (terrain.erode) report progress in 10 % steps.
    s.stack.context().progress = [&s, last = -1](float done) mutable {
        const int step = static_cast<int>(done * 10.0f);
        if (step != last) {
            last = step;
            s.say("  {}%", step * 10);
        }
        return true;
    };
    if (plugin_dir) {
        for (const api::PluginRecord& plugin : s.plugins.load_directory(*plugin_dir)) {
            if (!plugin.loaded) {
                std::cerr << "forge3d-cli: plugin " << plugin.path.string() << " not loaded: " << plugin.error << '\n';
            }
        }
    }
    if (i >= args.size()) {
        std::cout << kUsage;
        return 0;
    }
    const auto next = [&](std::string_view what) -> std::optional<std::string> {
        if (i + 1 >= args.size()) {
            return std::nullopt;
        }
        (void)what;
        return std::string(args[++i]);
    };

    for (; i < args.size(); ++i) {
        const std::string_view command = args[i];
        if (command == "help" || command == "--help" || command == "-h") {
            std::cout << kUsage;
        } else if (command == "compare") {
            const auto a = next("image");
            const auto b = next("image");
            if (!a || !b) {
                return fail("compare expects two PNG files");
            }
            double min_ssim = 0.99;
            if (i + 1 < args.size() && args[i + 1] == "--ssim") {
                ++i;
                const auto value = next("ssim");
                if (!value) {
                    return fail("--ssim expects a number");
                }
                min_ssim = std::stod(*value);
            }
            const auto image_a = core::read_png(*a);
            const auto image_b = core::read_png(*b);
            if (!image_a || !image_b) {
                return fail("compare: cannot read " + (!image_a ? *a : *b));
            }
            const core::ImageComparison r = core::compare_images(*image_a, *image_b);
            const std::string summary =
                fmt::format("SSIM {:.5f} (min {:.3f}), worst block {:.4f}, max diff {}, {} pixels differ", r.ssim,
                            min_ssim, r.min_ssim, r.max_difference, r.differing_pixels);
            if (!r.same_size || r.ssim < min_ssim) {
                return fail("compare: images differ: " + (r.same_size ? summary : std::string("sizes differ")));
            }
            s.say("compare: {}", summary);
        } else if (command == "golden") {
            const auto spec = next("spec");
            if (!spec) {
                return fail("golden expects a spec (.json)");
            }
            bool update = false;
            std::filesystem::path out = "golden-output";
            while (i + 1 < args.size() && (args[i + 1] == "--update" || args[i + 1] == "--out")) {
                if (args[++i] == "--update") {
                    update = true;
                } else if (const auto dir = next("dir")) {
                    out = *dir;
                } else {
                    return fail("--out expects a directory");
                }
            }
            if (const int code = golden(s, *spec, update, out); code != 0) {
                return code;
            }
        } else if (command == "version") {
            std::cout << "forge3d " << core::version() << '\n';
        } else if (command == "project") {
            const auto dir = next("dir");
            if (!dir) {
                return fail("project expects a directory");
            }
            s.files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(*dir));
            s.say("project:// = {}", *dir);
        } else if (command == "new") {
            s.stack.clear();
            s.world.clear();
            s.say("new scene");
        } else if (command == "open") {
            const auto path = next("file");
            if (!path) {
                return fail("open expects a .forge file");
            }
            s.stack.clear();
            const io::ForgeResult r = io::load_forge(*path, s.world);
            if (!r.ok) {
                return fail(r.error);
            }
            s.say("opened {} (format v{}, {} entities)", *path, r.loaded_version, s.world.size());
        } else if (command == "save") {
            const auto path = next("file");
            if (!path) {
                return fail("save expects a .forge file");
            }
            const io::ForgeResult r = io::save_forge(*path, s.world);
            if (!r.ok) {
                return fail(r.error);
            }
            s.say("saved {} ({} entities)", *path, s.world.size());
        } else if (command == "run-op") {
            const auto id = next("id");
            if (!id) {
                return fail("run-op expects an operation id");
            }
            nlohmann::json params = nlohmann::json::object();
            if (i + 1 < args.size() && args[i + 1] == "--params") {
                ++i;
                const auto text = next("json");
                if (!text) {
                    return fail("--params expects a JSON object");
                }
                try {
                    params = nlohmann::json::parse(*text);
                } catch (const std::exception& e) {
                    return fail("run-op " + *id + ": invalid JSON: " + e.what());
                }
            }
            std::unique_ptr<ops::Operation> op;
            try {
                op = s.registry.create(*id, params);
            } catch (const std::exception& e) {
                return fail("run-op " + *id + ": bad parameters: " + e.what());
            }
            if (!op) {
                return fail("run-op: unknown operation '" + *id + "' (see list-ops)");
            }
            const std::string name = op->display_name();
            if (!s.stack.execute(std::move(op))) {
                return fail("run-op " + *id + ": failed (invalid parameters for the current scene)");
            }
            s.say("ran {}", name);
        } else if (command == "run") {
            const auto path = next("script");
            if (!path) {
                return fail("run expects an ops script (.json) or a Python script (.py)");
            }
            if (std::filesystem::path(*path).extension() == ".py") {
                if (const int code = s.run_python(*path); code != 0) {
                    return code;
                }
                continue;
            }
            const ops::ScriptResult r = ops::run_script_file(s.stack, s.registry, *path, "Run " + *path);
            if (!r.ok) {
                return fail("run " + *path + ": " + r.error);
            }
            s.say("ran {} operations from {}", r.executed, *path);
        } else if (command == "undo") {
            if (!s.stack.undo()) {
                return fail("undo: nothing to undo");
            }
            s.say("undone");
        } else if (command == "redo") {
            if (!s.stack.redo()) {
                return fail("redo: nothing to redo");
            }
            s.say("redone");
        } else if (command == "list-ops") {
            const bool as_json = i + 1 < args.size() && args[i + 1] == "--json";
            if (as_json) {
                ++i;
            }
            list_ops(s, as_json);
        } else if (command == "print") {
            const std::string text = scene::snapshot_world(s.world).dump(2) + "\n";
            if (i + 1 < args.size() && args[i + 1] == "--out") {
                ++i;
                const auto path = next("file");
                if (!path) {
                    return fail("print --out expects a file");
                }
                std::ofstream out(*path, std::ios::binary);
                if (!(out << text)) {
                    return fail("cannot write " + *path);
                }
            } else {
                std::cout << text;
            }
        } else if (command == "import") {
            const auto dir = next("dir");
            if (!dir) {
                return fail("import expects a project directory");
            }
            const bool force = i + 1 < args.size() && args[i + 1] == "--force";
            if (force) {
                ++i;
            }
            core::vfs::FileSystem files;
            files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(*dir));
            assets::AssetDatabase db;
            const assets::ScanReport scan = db.rescan(files, "project://");
            for (const std::string& orphan : scan.orphaned) {
                std::cerr << "warning: orphaned " << orphan << '\n';
            }
            std::size_t imported = 0;
            std::size_t failed = 0;
            for (const assets::AssetRecord& record : db.records()) {
                if (!force && !record.stale()) {
                    continue;
                }
                const assets::ImportOutcome outcome = assets::import_asset(files, record);
                for (const std::string& warning : outcome.warnings) {
                    std::cerr << "warning: " << record.path << ": " << warning << '\n';
                }
                if (outcome.ok) {
                    ++imported;
                    s.say("imported {} -> {} file(s)", record.path, outcome.outputs.size());
                } else {
                    ++failed;
                    std::cerr << "forge3d-cli: import failed: " << outcome.error << '\n';
                }
            }
            s.say("{} assets, {} imported, {} failed, {} up to date", db.size(), imported, failed,
                  db.size() - imported - failed);
            if (failed > 0) {
                return 1;
            }
        } else if (command == "info") {
            std::cout << "entities: " << s.world.size() << "\nroots: " << s.world.roots().size()
                      << "\nlayers: " << s.world.layers().size()
                      << "\nbookmarks: " << s.world.bookmarks().bookmarks().size()
                      << "\nundo steps: " << s.stack.undo_count() << '\n';
        } else {
            return fail("unknown command '" + std::string(command) + "' (see help)");
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    f3d::core::disable_error_dialogs();
    f3d::core::log::init(f3d::core::LogLevel::Warn); // stdout is for command output
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    try {
        return run(args);
    } catch (const std::exception& e) {
        return fail(std::string("internal error: ") + e.what());
    }
}
