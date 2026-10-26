// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/rhi/shader_library.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/system.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d_shader_config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>

namespace f3d::render::rhi {
namespace {

std::string_view extension_for(Backend backend) noexcept {
    return backend == Backend::D3D12 ? ".dxil" : ".spv";
}

std::string quote(const std::string& s) {
    return "\"" + s + "\"";
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::transform(chars.begin(), chars.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

ShaderLibraryDesc default_shader_library_desc() {
    ShaderLibraryDesc desc;
    // Packaged builds ship shaders next to the executable; dev builds use the build tree.
    const auto beside_exe = core::executable_dir() / "shaders";
    std::error_code ec;
    desc.binary_dir =
        std::filesystem::is_directory(beside_exe, ec) ? beside_exe : std::filesystem::path(F3D_SHADER_BINARY_DIR);
#if defined(F3D_SHADER_HOT_RELOAD) && F3D_SHADER_HOT_RELOAD
    if (std::filesystem::is_directory(F3D_SHADER_SOURCE_DIR, ec) && std::filesystem::exists(F3D_DXC_EXECUTABLE, ec)) {
        desc.source_dir = F3D_SHADER_SOURCE_DIR;
        desc.dxc = F3D_DXC_EXECUTABLE;
    }
#endif
    return desc;
}

std::optional<ShaderStage> stage_from_name(std::string_view name) noexcept {
    if (name.ends_with(".vs")) {
        return ShaderStage::Vertex;
    }
    if (name.ends_with(".ps")) {
        return ShaderStage::Pixel;
    }
    if (name.ends_with(".cs")) {
        return ShaderStage::Compute;
    }
    return std::nullopt;
}

ShaderLibrary::ShaderLibrary(Device& device, ShaderLibraryDesc desc) : device_(device), desc_(std::move(desc)) {
    if (hot_reload_enabled()) {
        F3D_LOG_INFO("Shader hot reload: watching {}", desc_.source_dir.string());
    }
}

std::filesystem::path ShaderLibrary::binary_path(std::string_view name) const {
    return desc_.binary_dir / (std::string(name) + std::string(extension_for(device_.backend())));
}

Shader ShaderLibrary::load(std::string_view name) {
    F3D_ZONE;
    const auto stage = stage_from_name(name);
    if (!stage) {
        F3D_LOG_ERROR("Shader name '{}' must end in .vs, .ps or .cs", name);
        return {};
    }
    const auto path = binary_path(name);
    const auto bytes = read_file(path);
    if (bytes.empty()) {
        F3D_LOG_ERROR("Shader '{}' not found at {}", name, path.string());
        return {};
    }
    if (std::find(loaded_.begin(), loaded_.end(), name) == loaded_.end()) {
        loaded_.emplace_back(name);
        watch_sources();
    }
    return device_.create_shader(*stage, bytes, name);
}

void ShaderLibrary::watch_sources() {
    if (!hot_reload_enabled()) {
        return;
    }
    for (const auto& name : loaded_) {
        watcher_.watch(desc_.source_dir / (name + ".hlsl"));
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(desc_.source_dir, ec)) {
        if (entry.path().extension() == ".hlsli") {
            watcher_.watch(entry.path());
        }
    }
}

std::vector<std::string> ShaderLibrary::poll() {
    std::vector<std::string> reloaded;
    if (!hot_reload_enabled()) {
        return reloaded;
    }
    const auto changed = watcher_.poll();
    if (changed.empty()) {
        return reloaded;
    }
    const bool include_changed =
        std::any_of(changed.begin(), changed.end(), [](const auto& p) { return p.extension() == ".hlsli"; });
    for (const auto& name : loaded_) {
        const auto source = desc_.source_dir / (name + ".hlsl");
        if (!include_changed && std::find(changed.begin(), changed.end(), source) == changed.end()) {
            continue;
        }
        const auto stage = stage_from_name(name);
        if (const auto error = compile(desc_.dxc, source, binary_path(name), *stage, device_.backend())) {
            F3D_LOG_ERROR("Shader hot reload: {} failed:\n{}", name, *error);
        } else {
            F3D_LOG_INFO("Shader hot reload: {} recompiled", name);
            reloaded.push_back(name);
        }
    }
    if (!reloaded.empty()) {
        ++generation_;
    }
    return reloaded;
}

std::optional<std::string> ShaderLibrary::compile(const std::filesystem::path& dxc, const std::filesystem::path& source,
                                                  const std::filesystem::path& output, ShaderStage stage,
                                                  Backend backend) {
    std::string cmd = quote(dxc.string());
    for (const char* flag : {F3D_DXC_FLAG_LIST}) {
        cmd += " " + quote(flag);
    }
    if (backend == Backend::Vulkan) {
        for (const char* flag : {F3D_DXC_SPIRV_FLAG_LIST}) {
            cmd += " " + quote(flag);
        }
    }
    cmd += " -T " + std::string(to_string(stage)) + "_6_0";
    const auto tmp_output = output.string() + ".tmp";
    const auto log = output.string() + ".log";
    cmd += " -Fo " + quote(tmp_output) + " " + quote(source.string()) + " > " + quote(log) + " 2>&1";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\""; // cmd.exe strips one level of outer quotes
#endif
    const int rc = std::system(cmd.c_str());
    std::string text = read_text(log);
    std::error_code ec;
    std::filesystem::remove(log, ec);
    if (rc != 0) {
        std::filesystem::remove(tmp_output, ec);
        return text.empty() ? "dxc exited with code " + std::to_string(rc) : text;
    }
    std::filesystem::rename(tmp_output, output, ec);
    if (ec) {
        return "could not replace " + output.string() + ": " + ec.message();
    }
    return std::nullopt;
}

} // namespace f3d::render::rhi
