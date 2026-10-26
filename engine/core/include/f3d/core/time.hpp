// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace f3d::core {

/// Seconds since the operating system created this process (includes loader
/// and DLL start-up time, so it measures cold start). Falls back to the time
/// since static initialisation of f3d_core if the OS query fails.
[[nodiscard]] double seconds_since_process_start() noexcept;

} // namespace f3d::core
