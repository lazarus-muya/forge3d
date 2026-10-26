// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

namespace f3d::editor {

/// Linear-space clear colour of the main window behind the UI.
inline constexpr std::array<float, 4> kWindowClearColor = {0.10f, 0.12f, 0.16f, 1.0f};

/// Linear-space background of the 3D viewport (tests check captured pixels against it).
inline constexpr std::array<float, 4> kViewportClearColor = {0.050f, 0.055f, 0.065f, 1.0f};

} // namespace f3d::editor
