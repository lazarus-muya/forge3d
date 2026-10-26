// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/system.hpp"

#if defined(_WIN32)
#include <Windows.h>
// Windows.h must come first.
#include <Psapi.h>
#include <crtdbg.h>
#include <cstdlib>
#else
#include <fstream>
#include <string>
#include <unistd.h>
#endif

namespace f3d::core {

void disable_error_dialogs() noexcept {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        (void)type; // the _Crt* calls are empty macros in release builds
    }
#endif
}

ProcessMemory process_memory() noexcept {
    ProcessMemory m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        m.working_set = counters.WorkingSetSize;
        m.peak_working_set = counters.PeakWorkingSetSize;
    }
#else
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        std::uint64_t kb = 0;
        if (key == "VmRSS:" && status >> kb) {
            m.working_set = kb * 1024;
        } else if (key == "VmHWM:" && status >> kb) {
            m.peak_working_set = kb * 1024;
        }
        status.ignore(1 << 16, '\n');
    }
#endif
    return m;
}

std::filesystem::path executable_dir() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : exe.parent_path();
#endif
}

} // namespace f3d::core
