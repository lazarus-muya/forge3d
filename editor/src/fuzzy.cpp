// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/fuzzy.hpp"

#include <algorithm>
#include <cctype>

namespace f3d::editor {
namespace {

constexpr int kMatch = 16;
constexpr int kWordStart = 24;
constexpr int kConsecutive = 16;
constexpr int kFirstChar = 12;
constexpr int kGap = 2; // per skipped character between matches (capped)
constexpr int kMaxGapPenalty = 12;
constexpr int kLeading = 3; // per unmatched character before the first match (capped)
constexpr int kMaxLeadingPenalty = 15;

char fold(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool is_word_start(std::string_view text, std::size_t i) {
    if (i == 0) {
        return true;
    }
    const char prev = text[i - 1];
    if (prev == ' ' || prev == '.' || prev == '_' || prev == '-' || prev == '/' || prev == ':') {
        return true;
    }
    return std::islower(static_cast<unsigned char>(prev)) && std::isupper(static_cast<unsigned char>(text[i]));
}

/// Greedy match of `pattern` in `text` starting with pattern[0] at `start`.
std::optional<int> score_from(std::string_view pattern, std::string_view text, std::size_t start) {
    int score = 0;
    std::size_t prev = std::string_view::npos;
    std::size_t t = start;
    for (std::size_t p = 0; p < pattern.size(); ++p) {
        while (t < text.size() && fold(text[t]) != fold(pattern[p])) {
            ++t;
        }
        if (t == text.size()) {
            return std::nullopt;
        }
        score += kMatch;
        if (is_word_start(text, t)) {
            score += kWordStart;
        }
        if (prev != std::string_view::npos) {
            if (t == prev + 1) {
                score += kConsecutive;
            } else {
                score -= std::min(static_cast<int>(t - prev - 1) * kGap, kMaxGapPenalty);
            }
        }
        prev = t;
        ++t;
    }
    if (start == 0) {
        score += kFirstChar;
    }
    score -= std::min(static_cast<int>(start) * kLeading, kMaxLeadingPenalty);
    return score;
}

} // namespace

std::optional<int> fuzzy_score(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) {
        return 0;
    }
    // Try every position of the first pattern character and keep the best alignment.
    std::optional<int> best;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (fold(text[i]) != fold(pattern[0])) {
            continue;
        }
        if (const auto s = score_from(pattern, text, i); s && (!best || *s > *best)) {
            best = s;
        }
    }
    return best;
}

std::vector<std::size_t> fuzzy_rank(std::string_view pattern, const std::vector<std::string>& candidates) {
    std::vector<std::pair<std::size_t, int>> scored;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (const auto s = fuzzy_score(pattern, candidates[i])) {
            scored.emplace_back(i, *s);
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [&](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        const auto& ta = candidates[a.first];
        const auto& tb = candidates[b.first];
        if (ta.size() != tb.size()) {
            return ta.size() < tb.size();
        }
        return ta < tb;
    });
    std::vector<std::size_t> out;
    out.reserve(scored.size());
    for (const auto& [i, s] : scored) {
        out.push_back(i);
    }
    return out;
}

} // namespace f3d::editor
