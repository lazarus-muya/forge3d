// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>

namespace f3d::core {

struct ProcessMemory {
    std::uint64_t working_set = 0;      ///< bytes currently resident (RSS)
    std::uint64_t peak_working_set = 0; ///< bytes, peak resident
};

/// Resident memory of this process (0 if the OS query fails).
[[nodiscard]] ProcessMemory process_memory() noexcept;

/// Directory containing the running executable (empty on failure).
[[nodiscard]] std::filesystem::path executable_dir();

/// For unattended runs (tests, the CLI, hidden/automated editor runs): failed
/// C runtime assertions and errors are written to stderr instead of opening a
/// modal dialog (which would hang a hidden process), and the system's crash UI
/// is suppressed. No effect outside Windows.
void disable_error_dialogs() noexcept;

} // namespace f3d::core
