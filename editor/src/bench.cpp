// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/bench.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <fstream>

namespace f3d::editor {
namespace {

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

constexpr double kMiB = 1024.0 * 1024.0;

std::string camera_json(const BenchReport& r) {
    if (!r.camera) {
        return {};
    }
    const auto& c = *r.camera;
    return fmt::format(R"(,
  "camera": {{
    "position": [{:.6f}, {:.6f}, {:.6f}],
    "target": [{:.6f}, {:.6f}, {:.6f}],
    "yaw": {:.6f}, "pitch": {:.6f}, "distance": {:.6f},
    "orthographic": {}, "navigation_preset": "{}"
  }})",
                       c.position.x, c.position.y, c.position.z, c.target.x, c.target.y, c.target.z, c.yaw, c.pitch,
                       c.distance, c.orthographic, escape(c.navigation_preset));
}

std::string residency_json(const BenchReport& r) {
    if (!r.residency) {
        return {};
    }
    const auto& s = *r.residency;
    return fmt::format(R"(,
  "residency": {{ "budget_mb": {:.1f}, "used_mb": {:.1f}, "peak_mb": {:.1f}, "meshes": {}, "textures": {} }})",
                       static_cast<double>(s.budget_bytes) / kMiB, static_cast<double>(s.used_bytes) / kMiB,
                       static_cast<double>(s.peak_bytes) / kMiB, s.meshes, s.textures);
}

std::string terrain_json(const BenchReport& r) {
    if (!r.terrain) {
        return {};
    }
    return fmt::format(R"(,
  "terrain": {{ "nodes": {}, "sculpt_strokes": {} }})",
                       r.terrain->nodes, r.terrain->sculpt_strokes);
}

} // namespace

std::string to_json(const BenchReport& r) {
    std::string gpu;
    for (std::size_t i = 0; i < r.gpu_pass_ms.size(); ++i) {
        gpu += fmt::format("{}\n      \"{}\": {:.4f}", i ? "," : "", escape(r.gpu_pass_ms[i].first),
                           r.gpu_pass_ms[i].second);
    }
    const auto& c = r.cpu_frame_ms;
    return fmt::format(R"({{
  "backend": "{}",
  "adapter": "{}",
  "resolution": [{}, {}],
  "viewport": [{}, {}],
  "vsync": {},
  "frames": {},
  "time_to_first_frame_ms": {:.1f},
  "cpu_frame_ms": {{ "avg": {:.4f}, "p50": {:.4f}, "p95": {:.4f}, "p99": {:.4f}, "min": {:.4f}, "max": {:.4f}, "samples": {} }},
  "gpu_pass_ms": {{{}{}}},
  "ram_mb": {{ "working_set": {:.1f}, "peak": {:.1f} }},
  "vram_mb": {{ "used": {:.1f}, "budget": {:.1f} }}{}{}{}
}}
)",
                       escape(r.backend), escape(r.adapter), r.width, r.height, r.viewport_width, r.viewport_height,
                       r.vsync, r.frames, r.time_to_first_frame_ms, c.average, c.p50, c.p95, c.p99, c.min, c.max,
                       c.count, gpu, gpu.empty() ? "" : "\n    ", static_cast<double>(r.ram_bytes) / kMiB,
                       static_cast<double>(r.ram_peak_bytes) / kMiB, static_cast<double>(r.vram_bytes) / kMiB,
                       static_cast<double>(r.vram_budget_bytes) / kMiB, residency_json(r), camera_json(r),
                       terrain_json(r));
}

bool write_bench_report(const std::filesystem::path& path, const BenchReport& report) {
    std::ofstream out(path, std::ios::binary);
    out << to_json(report);
    return static_cast<bool>(out);
}

void FrameStats::add_frame(double cpu_ms, const std::vector<std::pair<std::string, double>>& gpu_ms) {
    ++frames_;
    if (frames_ <= warmup_) {
        return;
    }
    cpu_ms_.push_back(cpu_ms);
    for (const auto& [name, ms] : gpu_ms) {
        auto it = std::find_if(gpu_.begin(), gpu_.end(), [&](const auto& e) { return e.first == name; });
        if (it == gpu_.end()) {
            gpu_.push_back({name, {ms, 1}});
        } else {
            it->second.first += ms;
            ++it->second.second;
        }
    }
}

core::Summary FrameStats::cpu_summary() const {
    return core::summarize(cpu_ms_);
}

std::vector<std::pair<std::string, double>> FrameStats::gpu_averages() const {
    std::vector<std::pair<std::string, double>> out;
    for (const auto& [name, acc] : gpu_) {
        out.emplace_back(name, acc.first / static_cast<double>(acc.second));
    }
    return out;
}

} // namespace f3d::editor
