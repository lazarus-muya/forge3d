// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/assert.hpp"

#include "f3d/core/log.hpp"

#include <cstdlib>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <csignal>
#include <fstream>
#include <string>
#endif

namespace f3d::core {

bool is_debugger_attached() noexcept {
#if defined(_WIN32)
    return IsDebuggerPresent() != 0;
#else
    // Linux: a non-zero TracerPid in /proc/self/status means a debugger is attached.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("TracerPid:", 0) == 0) {
            return std::strtol(line.c_str() + 10, nullptr, 10) != 0;
        }
    }
    return false;
#endif
}

void break_if_debugger_attached() noexcept {
    if (!is_debugger_attached()) {
        return;
    }
#if defined(_WIN32)
    __debugbreak();
#else
    std::raise(SIGTRAP);
#endif
}

namespace detail {

void assert_failed(const char* expr, std::string_view msg, const char* file, int line) noexcept {
    F3D_LOG_CRITICAL("Assertion failed: {} ({}) at {}:{}", msg, expr, file, line);
    log::logger().flush();
    break_if_debugger_attached();
    std::abort();
}

bool verify_failed(const char* expr, std::string_view msg, const char* file, int line) noexcept {
    F3D_LOG_ERROR("Verify failed: {} ({}) at {}:{}", msg, expr, file, line);
    break_if_debugger_attached();
    return false;
}

} // namespace detail
} // namespace f3d::core
