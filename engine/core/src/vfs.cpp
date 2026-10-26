// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/vfs.hpp"

#include "f3d/core/profile.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace f3d::core::vfs {
namespace {

std::filesystem::path to_fs(std::string_view relative) {
    return std::filesystem::path(std::u8string(relative.begin(), relative.end()));
}

std::string to_generic(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

// --- Backend defaults ---------------------------------------------------------------------------

bool Backend::write(std::string_view, const Bytes&) {
    return false;
}

bool Backend::remove(std::string_view) {
    return false;
}

std::optional<std::filesystem::path> Backend::native_path(std::string_view) const {
    return std::nullopt;
}

// --- Paths --------------------------------------------------------------------------------------

std::optional<VPath> parse(std::string_view path) {
    const auto sep = path.find("://");
    if (sep == std::string_view::npos || sep == 0) {
        return std::nullopt;
    }
    VPath out;
    out.mount = std::string(path.substr(0, sep));
    std::vector<std::string> segments;
    std::string segment;
    const auto flush = [&]() -> bool {
        if (segment.empty() || segment == ".") {
            // skip
        } else if (segment == "..") {
            if (segments.empty()) {
                return false; // would leave the mount
            }
            segments.pop_back();
        } else {
            segments.push_back(segment);
        }
        segment.clear();
        return true;
    };
    for (const char c : path.substr(sep + 3)) {
        if (c == '/' || c == '\\') {
            if (!flush()) {
                return std::nullopt;
            }
        } else if (c == ':') {
            return std::nullopt; // no drive letters or nested schemes
        } else {
            segment.push_back(c);
        }
    }
    if (!flush()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        out.relative += (i ? "/" : "") + segments[i];
    }
    return out;
}

std::string join(std::string_view mount, std::string_view relative) {
    return std::string(mount) + "://" + std::string(relative);
}

// --- Directory backend --------------------------------------------------------------------------

DirectoryBackend::DirectoryBackend(std::filesystem::path root, bool writable)
    : root_(std::move(root)), writable_(writable) {}

bool DirectoryBackend::exists(std::string_view path) const {
    std::error_code ec;
    return std::filesystem::exists(root_ / to_fs(path), ec);
}

std::optional<Bytes> DirectoryBackend::read(std::string_view path) const {
    F3D_ZONE;
    std::ifstream in(root_ / to_fs(path), std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    Bytes data(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }
    return data;
}

std::vector<FileInfo> DirectoryBackend::list(std::string_view dir, bool recursive) const {
    std::vector<FileInfo> out;
    const std::filesystem::path base = root_ / to_fs(dir);
    std::error_code ec;
    const auto add = [&](const std::filesystem::directory_entry& entry) {
        FileInfo info;
        info.path = to_generic(std::filesystem::relative(entry.path(), root_, ec));
        info.directory = entry.is_directory(ec);
        info.size = info.directory ? 0 : entry.file_size(ec);
        info.modified = static_cast<std::int64_t>(entry.last_write_time(ec).time_since_epoch().count());
        out.push_back(std::move(info));
    };
    if (recursive) {
        for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            add(*it);
        }
    } else {
        for (auto it = std::filesystem::directory_iterator(base, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            add(*it);
        }
    }
    std::sort(out.begin(), out.end(), [](const FileInfo& a, const FileInfo& b) { return a.path < b.path; });
    return out;
}

bool DirectoryBackend::write(std::string_view path, const Bytes& data) {
    if (!writable_ || path.empty()) {
        return false;
    }
    const std::filesystem::path target = root_ / to_fs(path);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
            return false;
        }
    }
    std::filesystem::rename(temp, target, ec);
    return !ec;
}

bool DirectoryBackend::remove(std::string_view path) {
    if (!writable_ || path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::remove(root_ / to_fs(path), ec);
}

std::optional<std::filesystem::path> DirectoryBackend::native_path(std::string_view path) const {
    return root_ / to_fs(path);
}

// --- File system --------------------------------------------------------------------------------

void FileSystem::mount(std::string mount, std::shared_ptr<Backend> backend) {
    std::scoped_lock lock(mutex_);
    mounts_[std::move(mount)] = std::move(backend);
}

void FileSystem::unmount(std::string_view mount) {
    std::scoped_lock lock(mutex_);
    mounts_.erase(std::string(mount));
}

bool FileSystem::mounted(std::string_view mount) const {
    std::scoped_lock lock(mutex_);
    return mounts_.contains(std::string(mount));
}

std::shared_ptr<Backend> FileSystem::backend(std::string_view mount) const {
    std::scoped_lock lock(mutex_);
    const auto it = mounts_.find(std::string(mount));
    return it == mounts_.end() ? nullptr : it->second;
}

std::shared_ptr<Backend> FileSystem::resolve(std::string_view path, VPath& parsed) const {
    const std::optional<VPath> p = parse(path);
    if (!p) {
        return nullptr;
    }
    parsed = *p;
    return backend(parsed.mount);
}

bool FileSystem::exists(std::string_view path) const {
    VPath p;
    const auto b = resolve(path, p);
    return b && b->exists(p.relative);
}

std::optional<Bytes> FileSystem::read(std::string_view path) const {
    VPath p;
    const auto b = resolve(path, p);
    return b ? b->read(p.relative) : std::nullopt;
}

std::optional<std::string> FileSystem::read_text(std::string_view path) const {
    std::optional<Bytes> data = read(path);
    if (!data) {
        return std::nullopt;
    }
    return std::string(data->begin(), data->end());
}

std::vector<FileInfo> FileSystem::list(std::string_view dir, bool recursive) const {
    VPath p;
    const auto b = resolve(dir, p);
    if (!b) {
        return {};
    }
    std::vector<FileInfo> out = b->list(p.relative, recursive);
    for (FileInfo& info : out) {
        info.path = join(p.mount, info.path);
    }
    return out;
}

bool FileSystem::write(std::string_view path, const Bytes& data) {
    VPath p;
    const auto b = resolve(path, p);
    return b && b->writable() && b->write(p.relative, data);
}

bool FileSystem::write_text(std::string_view path, std::string_view text) {
    return write(path, Bytes(text.begin(), text.end()));
}

bool FileSystem::remove(std::string_view path) {
    VPath p;
    const auto b = resolve(path, p);
    return b && b->writable() && b->remove(p.relative);
}

std::optional<std::filesystem::path> FileSystem::native_path(std::string_view path) const {
    VPath p;
    const auto b = resolve(path, p);
    return b ? b->native_path(p.relative) : std::nullopt;
}

std::optional<std::string> FileSystem::to_virtual(const std::filesystem::path& native) const {
    std::scoped_lock lock(mutex_);
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::weakly_canonical(native, ec);
    std::optional<std::string> best;
    std::size_t best_length = 0;
    for (const auto& [mount, backend] : mounts_) {
        const auto* dir = dynamic_cast<const DirectoryBackend*>(backend.get());
        if (!dir) {
            continue;
        }
        const std::filesystem::path root = std::filesystem::weakly_canonical(dir->root(), ec);
        const std::filesystem::path relative = target.lexically_relative(root);
        const std::string rel = to_generic(relative);
        if (relative.empty() || rel.starts_with("..")) {
            continue;
        }
        const std::size_t length = to_generic(root).size();
        if (!best || length > best_length) {
            best = join(mount, rel == "." ? std::string() : rel);
            best_length = length;
        }
    }
    return best;
}

} // namespace f3d::core::vfs
