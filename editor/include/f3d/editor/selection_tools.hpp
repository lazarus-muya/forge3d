// SPDX-License-Identifier: Apache-2.0
#pragma once

// How a click or box selection combines with the current selection (phases.md
// P2.4). Pure functions; the editor turns the result into a selection.set op.

#include "f3d/core/guid.hpp"

#include <vector>

namespace f3d::editor {

enum class SelectMode {
    Replace, ///< plain click / box
    Add,     ///< Shift + box
    Remove,  ///< Ctrl + click / box
    Toggle,  ///< Shift + click
};

struct SelectionResult {
    std::vector<core::Guid> entities;
    core::Guid active;
};

/// Combines `picked` (in pick order) with `current` according to `mode`. The
/// active entity becomes the first picked entity that ends up selected, or stays
/// as it was if it is still selected.
[[nodiscard]] SelectionResult combine_selection(const std::vector<core::Guid>& current, const core::Guid& active,
                                                const std::vector<core::Guid>& picked, SelectMode mode);

} // namespace f3d::editor
