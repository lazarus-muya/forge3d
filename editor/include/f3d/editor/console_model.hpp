// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/log.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::editor {

struct ConsoleEntry {
    core::LogLevel level = core::LogLevel::Info;
    std::string time; ///< HH:MM:SS.mmm, local time
    std::string message;
};

/// Model behind the console panel: a bounded, thread-safe log history that
/// subscribes to the core log as a LogSink. The panel only reads it.
class ConsoleModel final : public core::LogSink {
public:
    explicit ConsoleModel(std::size_t capacity = 10'000) : capacity_(capacity) {}

    void write(const core::LogRecord& record) override;

    void clear();
    [[nodiscard]] std::size_t size() const;
    /// Increases on every write or clear; lets the panel auto-scroll on new lines.
    [[nodiscard]] std::uint64_t version() const;

    /// Entries at or above `min_level` whose message contains `search`
    /// (case-insensitive), oldest first.
    [[nodiscard]] std::vector<ConsoleEntry> filtered(core::LogLevel min_level, std::string_view search) const;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<ConsoleEntry> entries_;
    std::uint64_t version_ = 0;
};

} // namespace f3d::editor
