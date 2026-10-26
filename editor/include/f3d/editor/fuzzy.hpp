// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::editor {

/// Fuzzy subsequence match for the command palette (case-insensitive).
/// nullopt if `pattern` is not a subsequence of `text`; otherwise a score
/// where higher is better. Bonuses: matches at word starts (after space, '.',
/// '_', '-', '/' or a lower->upper case change), consecutive matches, a match
/// at the very start. Penalties: gaps and unmatched leading characters.
/// An empty pattern matches everything with score 0.
[[nodiscard]] std::optional<int> fuzzy_score(std::string_view pattern, std::string_view text);

/// Indices of `candidates` that match `pattern`, best first (ties: shorter, then alphabetical).
[[nodiscard]] std::vector<std::size_t> fuzzy_rank(std::string_view pattern, const std::vector<std::string>& candidates);

} // namespace f3d::editor
