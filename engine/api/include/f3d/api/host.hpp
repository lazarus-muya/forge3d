// SPDX-License-Identifier: Apache-2.0
#pragma once

// The C++ side of the C API (forge3d_api.h, phases.md P5.1): a Host wraps an
// editor or CLI session (operation registry, history, world) for C callers, and
// the PluginLoader loads plugin libraries into it.

#include "forge3d_api.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace f3d::scene {
class World;
}
namespace f3d::ops {
class OpRegistry;
class OpStack;
} // namespace f3d::ops

namespace f3d::api {

/// What the C API acts on. The handle passed to C code is this object.
class Host {
public:
    Host(ops::OpRegistry& registry, ops::OpStack& stack, scene::World& world) noexcept
        : registry_(registry), stack_(stack), world_(world) {}

    [[nodiscard]] ops::OpRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] ops::OpStack& stack() noexcept { return stack_; }
    [[nodiscard]] scene::World& world() noexcept { return world_; }

    [[nodiscard]] F3dHost* handle() noexcept { return reinterpret_cast<F3dHost*>(this); }
    [[nodiscard]] static Host* from(F3dHost* handle) noexcept { return reinterpret_cast<Host*>(handle); }

private:
    ops::OpRegistry& registry_;
    ops::OpStack& stack_;
    scene::World& world_;
};

/// Registers a C operation (F3dOpDesc) with `registry`: undoable, its scene changes
/// recorded as child operations. Fails (false, `error`) on a missing id or execute
/// function, a duplicate id, or an invalid parameter schema.
bool register_c_operation(ops::OpRegistry& registry, const F3dOpDesc& desc, std::string* error = nullptr);

struct PluginRecord {
    std::filesystem::path path;
    std::string name;    ///< from the plugin (the file name if it didn't say)
    std::string version; ///< from the plugin
    bool loaded = false;
    std::string error; ///< why it wasn't loaded
};

/// Loads plugin libraries into a Host. Libraries stay loaded for the life of the
/// process (their operations may sit in an undo history).
class PluginLoader {
public:
    explicit PluginLoader(Host& host) noexcept : host_(host) {}

    /// Loads one library: checks f3d_plugin_api_version against this host before
    /// running any of its code, then calls f3d_plugin_entry.
    PluginRecord load(const std::filesystem::path& library);
    /// Loads every shared library (.dll / .so / .dylib) in `dir` (none if it doesn't exist).
    std::vector<PluginRecord> load_directory(const std::filesystem::path& dir);
    [[nodiscard]] const std::vector<PluginRecord>& plugins() const noexcept { return plugins_; }

private:
    Host& host_;
    std::vector<PluginRecord> plugins_;
};

/// `plugins/` next to the running executable.
[[nodiscard]] std::filesystem::path default_plugin_dir();

/// True if a plugin built against `plugin_version` can use this host's API.
[[nodiscard]] bool is_compatible(std::uint32_t plugin_version) noexcept;

} // namespace f3d::api
