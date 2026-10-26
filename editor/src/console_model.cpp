// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/console_model.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>

#include <fmt/format.h>

namespace f3d::editor {
namespace {

std::string format_time(std::chrono::system_clock::time_point time) {
    const std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count() % 1000;
    return fmt::format("{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min, local.tm_sec, ms);
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

} // namespace

void ConsoleModel::write(const core::LogRecord& record) {
    ConsoleEntry entry{record.level, format_time(record.time), std::string(record.message)};
    std::scoped_lock lock(mutex_);
    entries_.push_back(std::move(entry));
    while (entries_.size() > capacity_) {
        entries_.pop_front();
    }
    ++version_;
}

void ConsoleModel::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    ++version_;
}

std::size_t ConsoleModel::size() const {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

std::uint64_t ConsoleModel::version() const {
    std::scoped_lock lock(mutex_);
    return version_;
}

std::vector<ConsoleEntry> ConsoleModel::filtered(core::LogLevel min_level, std::string_view search) const {
    std::scoped_lock lock(mutex_);
    std::vector<ConsoleEntry> out;
    for (const ConsoleEntry& e : entries_) {
        if (e.level >= min_level && contains_case_insensitive(e.message, search)) {
            out.push_back(e);
        }
    }
    return out;
}

} // namespace f3d::editor
