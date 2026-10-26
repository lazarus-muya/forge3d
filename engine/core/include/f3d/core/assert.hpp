// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

// F3D_ASSERT(cond, msg)  Debug builds only. On failure: logs, breaks into an
//                        attached debugger, then aborts. Compiled out (cond is
//                        not evaluated) when F3D_ENABLE_ASSERTS is 0.
// F3D_VERIFY(cond, msg)  Always on. On failure: logs an error and breaks into an
//                        attached debugger, then continues. Evaluates to the
//                        result of `cond`, so it can guard a fallback:
//                            if (!F3D_VERIFY(ptr, "missing ptr")) return;

#ifndef F3D_ENABLE_ASSERTS
#ifdef NDEBUG
#define F3D_ENABLE_ASSERTS 0
#else
#define F3D_ENABLE_ASSERTS 1
#endif
#endif

namespace f3d::core {

/// True if a debugger is attached to this process.
[[nodiscard]] bool is_debugger_attached() noexcept;

/// Breaks into the debugger if one is attached; does nothing otherwise.
void break_if_debugger_attached() noexcept;

namespace detail {
[[noreturn]] void assert_failed(const char* expr, std::string_view msg, const char* file, int line) noexcept;
bool verify_failed(const char* expr, std::string_view msg, const char* file, int line) noexcept;
} // namespace detail

} // namespace f3d::core

#if F3D_ENABLE_ASSERTS
#define F3D_ASSERT(cond, msg)                                                                                          \
    do {                                                                                                               \
        if (!(cond)) [[unlikely]] {                                                                                    \
            ::f3d::core::detail::assert_failed(#cond, (msg), __FILE__, __LINE__);                                      \
        }                                                                                                              \
    } while (false)
#else
#define F3D_ASSERT(cond, msg)                                                                                          \
    do {                                                                                                               \
        (void)sizeof(!(cond));                                                                                         \
    } while (false)
#endif

#define F3D_VERIFY(cond, msg)                                                                                          \
    (static_cast<bool>(cond) ? true : ::f3d::core::detail::verify_failed(#cond, (msg), __FILE__, __LINE__))
