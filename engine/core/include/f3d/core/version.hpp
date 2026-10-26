// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace f3d::core {

/// forge3d's version (CMake project version), e.g. "0.1.0". Written into saved files.
[[nodiscard]] std::string_view version() noexcept;

} // namespace f3d::core
