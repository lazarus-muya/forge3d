// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <spdlog/logger.h>

#include <chrono>
#include <memory>
#include <string_view>

namespace f3d::core {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Critical, Off };

/// One formatted log message, as delivered to a LogSink. The views are only
/// valid for the duration of LogSink::write.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::chrono::system_clock::time_point time;
    std::string_view message;
};

/// Receives every log message at or above the global level. Implementations
/// must be thread-safe: write() may be called from any thread, but never
/// concurrently for the same sink.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

} // namespace f3d::core

namespace f3d::core::log {

/// Installs the default stderr sink. Safe to call more than once; logging
/// before init() initialises lazily with the defaults.
void init(LogLevel level = LogLevel::Info);

/// Flushes all sinks and removes every sink added with add_sink().
void shutdown();

void set_level(LogLevel level);
[[nodiscard]] LogLevel level();

/// Adds a sink to the sink list (e.g. the editor console panel).
void add_sink(std::shared_ptr<LogSink> sink);
void remove_sink(const std::shared_ptr<LogSink>& sink);

/// Enables or disables the default stderr sink (tests mute it).
void set_console_enabled(bool enabled);

/// The underlying spdlog logger. Use the F3D_LOG_* macros instead.
[[nodiscard]] spdlog::logger& logger();

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

} // namespace f3d::core::log

#define F3D_LOG_TRACE(...) ::f3d::core::log::logger().trace(__VA_ARGS__)
#define F3D_LOG_DEBUG(...) ::f3d::core::log::logger().debug(__VA_ARGS__)
#define F3D_LOG_INFO(...) ::f3d::core::log::logger().info(__VA_ARGS__)
#define F3D_LOG_WARN(...) ::f3d::core::log::logger().warn(__VA_ARGS__)
#define F3D_LOG_ERROR(...) ::f3d::core::log::logger().error(__VA_ARGS__)
#define F3D_LOG_CRITICAL(...) ::f3d::core::log::logger().critical(__VA_ARGS__)
