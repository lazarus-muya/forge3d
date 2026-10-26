// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/project_import.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

namespace f3d::editor {
namespace fs = std::filesystem;
namespace {

bool same_contents(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) {
        return false;
    }
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    std::array<char, 64 * 1024> ba{};
    std::array<char, 64 * 1024> bb{};
    while (fa && fb) {
        fa.read(ba.data(), ba.size());
        fb.read(bb.data(), bb.size());
        if (fa.gcount() != fb.gcount() || !std::equal(ba.begin(), ba.begin() + fa.gcount(), bb.begin())) {
            return false;
        }
    }
    return fa.eof() && fb.eof();
}

std::string percent_decode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

/// A .gltf's external buffers (the importer loads them, so they aren't in its dependencies).
std::vector<std::string> gltf_buffers(const fs::path& model) {
    std::vector<std::string> out;
    std::string ext = model.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".gltf") {
        return out;
    }
    try {
        std::ifstream in(model, std::ios::binary);
        const nlohmann::json gltf = nlohmann::json::parse(in);
        for (const auto& buffer : gltf.value("buffers", nlohmann::json::array())) {
            const std::string uri = buffer.value("uri", std::string());
            if (!uri.empty() && uri.rfind("data:", 0) != 0) {
                out.push_back(percent_decode(uri));
            }
        }
    } catch (const std::exception&) {
        // Not parseable here: the importer already read it, so just copy without buffers.
    }
    return out;
}

bool escapes(const fs::path& relative) {
    if (relative.is_absolute() || relative.has_root_name()) {
        return true;
    }
    return std::any_of(relative.begin(), relative.end(), [](const fs::path& part) { return part == ".."; });
}

} // namespace

bool is_inside(const fs::path& path, const fs::path& dir) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(path, ec);
    const fs::path d = fs::weakly_canonical(dir, ec);
    if (ec || d.empty()) {
        return false;
    }
    const fs::path relative = p.lexically_relative(d);
    return !relative.empty() && !escapes(relative);
}

std::optional<fs::path> copy_model_into_project(const fs::path& model, const std::vector<std::string>& dependencies,
                                                const fs::path& project_dir, std::string* error,
                                                std::vector<std::string>* warnings) {
    const auto fail = [&](std::string why) -> std::optional<fs::path> {
        if (error) {
            *error = std::move(why);
        }
        return std::nullopt;
    };
    std::error_code ec;
    if (!fs::is_regular_file(model, ec)) {
        return fail(model.string() + " is not a file");
    }
    const fs::path source_dir = model.parent_path();
    std::set<std::string> files;
    for (const std::string& dep : dependencies) {
        files.insert(fs::path(dep).lexically_normal().generic_string());
    }
    for (const std::string& buffer : gltf_buffers(model)) {
        files.insert(fs::path(buffer).lexically_normal().generic_string());
    }

    // <stem>/, or <stem>_2/... when a different model of that name is already there.
    const fs::path imported = project_dir / "Imported";
    const std::string stem = model.stem().string();
    fs::path target_dir;
    for (int n = 1;; ++n) {
        const fs::path dir = imported / (n == 1 ? stem : stem + "_" + std::to_string(n));
        const fs::path existing = dir / model.filename();
        if (!fs::exists(dir, ec)) {
            target_dir = dir;
            break;
        }
        if (fs::exists(existing, ec) && same_contents(model, existing)) {
            return existing; // imported before
        }
        if (n > 999) {
            return fail("too many models named " + stem + " in " + imported.string());
        }
    }

    fs::create_directories(target_dir, ec);
    if (ec) {
        return fail("cannot create " + target_dir.string() + ": " + ec.message());
    }
    const fs::path target = target_dir / model.filename();
    fs::copy_file(model, target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return fail("cannot copy " + model.string() + ": " + ec.message());
    }
    for (const std::string& file : files) {
        const fs::path relative(file);
        const fs::path from = source_dir / relative;
        if (escapes(relative)) {
            if (warnings) {
                warnings->push_back(file + " is outside the model's folder; not copied");
            }
            continue;
        }
        if (!fs::is_regular_file(from, ec)) {
            if (warnings) {
                warnings->push_back(file + " is missing; not copied");
            }
            continue;
        }
        fs::create_directories((target_dir / relative).parent_path(), ec);
        fs::copy_file(from, target_dir / relative, fs::copy_options::overwrite_existing, ec);
        if (ec && warnings) {
            warnings->push_back("cannot copy " + file + ": " + ec.message());
        }
    }
    return target;
}

} // namespace f3d::editor
