// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/log.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace f3d::core::log {
namespace {

spdlog::level::level_enum to_spdlog(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warn:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    case LogLevel::Off:
        break;
    }
    return spdlog::level::off;
}

LogLevel from_spdlog(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace:
        return LogLevel::Trace;
    case spdlog::level::debug:
        return LogLevel::Debug;
    case spdlog::level::info:
        return LogLevel::Info;
    case spdlog::level::warn:
        return LogLevel::Warn;
    case spdlog::level::err:
        return LogLevel::Error;
    case spdlog::level::critical:
        return LogLevel::Critical;
    default:
        return LogLevel::Off;
    }
}

/// Forwards spdlog messages to a LogSink.
class SinkAdapter final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit SinkAdapter(std::shared_ptr<LogSink> target) : target_(std::move(target)) {}
    [[nodiscard]] const std::shared_ptr<LogSink>& target() const { return target_; }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        target_->write(LogRecord{from_spdlog(msg.level), msg.time, {msg.payload.data(), msg.payload.size()}});
    }
    void flush_() override { target_->flush(); }

private:
    std::shared_ptr<LogSink> target_;
};

struct State {
    std::shared_ptr<spdlog::sinks::dist_sink_mt> dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
    std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> console =
        std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    std::shared_ptr<spdlog::logger> logger;
    std::mutex mutex; // guards adapters and console_enabled
    std::vector<std::shared_ptr<SinkAdapter>> adapters;
    bool console_enabled = true;

    State() {
        console->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        dist->add_sink(console);
        logger = std::make_shared<spdlog::logger>("forge3d", dist);
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);
    }
};

State& state() {
    static State s;
    return s;
}

} // namespace

void init(LogLevel level) {
    set_level(level);
}

void shutdown() {
    State& s = state();
    s.logger->flush();
    std::scoped_lock lock(s.mutex);
    for (const auto& adapter : s.adapters) {
        s.dist->remove_sink(adapter);
    }
    s.adapters.clear();
}

void set_level(LogLevel level) {
    state().logger->set_level(to_spdlog(level));
}

LogLevel level() {
    return from_spdlog(state().logger->level());
}

void add_sink(std::shared_ptr<LogSink> sink) {
    if (!sink) {
        return;
    }
    State& s = state();
    auto adapter = std::make_shared<SinkAdapter>(std::move(sink));
    adapter->set_pattern("%v");
    std::scoped_lock lock(s.mutex);
    s.adapters.push_back(adapter);
    s.dist->add_sink(adapter);
}

void remove_sink(const std::shared_ptr<LogSink>& sink) {
    State& s = state();
    std::scoped_lock lock(s.mutex);
    auto it = std::find_if(s.adapters.begin(), s.adapters.end(), [&](const auto& a) { return a->target() == sink; });
    if (it != s.adapters.end()) {
        s.dist->remove_sink(*it);
        s.adapters.erase(it);
    }
}

void set_console_enabled(bool enabled) {
    State& s = state();
    std::scoped_lock lock(s.mutex);
    if (enabled == s.console_enabled) {
        return;
    }
    s.console_enabled = enabled;
    if (enabled) {
        s.dist->add_sink(s.console);
    } else {
        s.dist->remove_sink(s.console);
    }
}

spdlog::logger& logger() {
    return *state().logger;
}

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    case LogLevel::Off:
        break;
    }
    return "off";
}

} // namespace f3d::core::log
