// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/assert.hpp"
#include "f3d/core/log.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct CaptureSink final : f3d::core::LogSink {
    std::mutex mutex;
    std::vector<std::pair<f3d::core::LogLevel, std::string>> lines;
    void write(const f3d::core::LogRecord& r) override {
        std::scoped_lock lock(mutex);
        lines.emplace_back(r.level, std::string(r.message));
    }
};

/// Mutes the console and restores the log level for the duration of a test.
struct LogFixture {
    f3d::core::LogLevel saved = f3d::core::log::level();
    std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    LogFixture() {
        f3d::core::log::set_console_enabled(false);
        f3d::core::log::add_sink(sink);
    }
    ~LogFixture() {
        f3d::core::log::remove_sink(sink);
        f3d::core::log::set_level(saved);
        f3d::core::log::set_console_enabled(true);
    }
};

} // namespace

TEST_CASE_FIXTURE(LogFixture, "log: sinks receive formatted messages at or above the level") {
    f3d::core::log::set_level(f3d::core::LogLevel::Info);
    F3D_LOG_TRACE("hidden {}", 1);
    F3D_LOG_INFO("hello {} {}", "world", 42);
    F3D_LOG_WARN("careful");
    F3D_LOG_ERROR("broken: {}", 3.5);

    REQUIRE(sink->lines.size() == 3);
    CHECK(sink->lines[0].first == f3d::core::LogLevel::Info);
    CHECK(sink->lines[0].second == "hello world 42");
    CHECK(sink->lines[1].first == f3d::core::LogLevel::Warn);
    CHECK(sink->lines[2].second == "broken: 3.5");

    f3d::core::log::set_level(f3d::core::LogLevel::Trace);
    F3D_LOG_TRACE("now visible");
    REQUIRE(sink->lines.size() == 4);
    CHECK(sink->lines[3].first == f3d::core::LogLevel::Trace);
}

TEST_CASE_FIXTURE(LogFixture, "log: removed sinks stop receiving") {
    f3d::core::log::remove_sink(sink);
    F3D_LOG_ERROR("not captured");
    CHECK(sink->lines.empty());
}

TEST_CASE_FIXTURE(LogFixture, "F3D_VERIFY returns the condition and logs failures") {
    int calls = 0;
    auto count = [&] {
        ++calls;
        return true;
    };
    CHECK(F3D_VERIFY(count(), "counted"));
    CHECK(calls == 1); // evaluated exactly once
    CHECK(sink->lines.empty());

    const bool ok = F3D_VERIFY(1 + 1 == 3, "math is broken");
    CHECK_FALSE(ok);
    REQUIRE(sink->lines.size() == 1);
    CHECK(sink->lines[0].first == f3d::core::LogLevel::Error);
    CHECK(sink->lines[0].second.find("math is broken") != std::string::npos);
    CHECK(sink->lines[0].second.find("1 + 1 == 3") != std::string::npos);
}

TEST_CASE("F3D_ASSERT passes on true conditions") {
    F3D_ASSERT(true, "never fires");
    CHECK(true);
}

TEST_CASE("log level names") {
    CHECK(f3d::core::log::to_string(f3d::core::LogLevel::Warn) == "warn");
    CHECK(f3d::core::log::to_string(f3d::core::LogLevel::Off) == "off");
}
