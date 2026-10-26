// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/time.hpp"

#include <chrono>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#endif

namespace f3d::core {
namespace {

const auto g_static_init_time = std::chrono::steady_clock::now();

double fallback() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_static_init_time).count();
}

} // namespace

double seconds_since_process_start() noexcept {
#if defined(_WIN32)
    FILETIME creation{}, exit_time{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
        return fallback();
    }
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const auto to_u64 = [](const FILETIME& ft) {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    return static_cast<double>(to_u64(now) - to_u64(creation)) * 1e-7; // 100 ns units
#else
    // Field 22 of /proc/self/stat is the start time in clock ticks after boot;
    // /proc/uptime gives seconds since boot.
    std::ifstream stat_file("/proc/self/stat");
    std::ifstream uptime_file("/proc/uptime");
    std::string stat;
    double uptime = 0.0;
    if (!std::getline(stat_file, stat) || !(uptime_file >> uptime)) {
        return fallback();
    }
    const auto after_comm = stat.rfind(')'); // the command name may contain spaces
    if (after_comm == std::string::npos) {
        return fallback();
    }
    std::istringstream fields(stat.substr(after_comm + 2));
    std::string field;
    for (int i = 3; i <= 22 && fields >> field; ++i) {
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    if (field.empty() || ticks <= 0) {
        return fallback();
    }
    return uptime - std::stod(field) / static_cast<double>(ticks);
#endif
}

} // namespace f3d::core
