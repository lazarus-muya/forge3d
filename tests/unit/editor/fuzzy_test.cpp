// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/fuzzy.hpp"

#include <string>
#include <vector>

using f3d::editor::fuzzy_rank;
using f3d::editor::fuzzy_score;

namespace {

const std::vector<std::string> kCommands = {
    "Reset Layout",    "Toggle Console", "Toggle Outliner", "Toggle Asset Browser", "Quit",
    "Command Palette", "Preferences",    "Keymap",          "Open glTF...",         "Toggle Orthographic",
    "Frame Selection", "View Front",     "View Top",        "Toggle Inspector",     "Toggle Viewport",
};

std::string top(std::string_view pattern) {
    const auto ranked = fuzzy_rank(pattern, kCommands);
    return ranked.empty() ? std::string("<none>") : kCommands[ranked.front()];
}

} // namespace

TEST_CASE("fuzzy: subsequence matching") {
    CHECK(fuzzy_score("", "anything") == 0);
    CHECK(fuzzy_score("rl", "Reset Layout").has_value());
    CHECK(fuzzy_score("RESET", "reset layout").has_value()); // case-insensitive
    CHECK_FALSE(fuzzy_score("xyz", "Reset Layout").has_value());
    CHECK_FALSE(fuzzy_score("layoutx", "Reset Layout").has_value());
}

TEST_CASE("fuzzy: word starts and contiguous runs beat scattered matches") {
    // "tc" hits two word starts in "Toggle Console".
    REQUIRE(fuzzy_score("tc", "Toggle Inspector"));
    CHECK(*fuzzy_score("tc", "Toggle Console") > *fuzzy_score("tc", "Toggle Inspector"));
    // A prefix beats the same letters in the middle.
    REQUIRE(fuzzy_score("view", "Toggle Viewport"));
    CHECK(*fuzzy_score("view", "View Front") > *fuzzy_score("view", "Toggle Viewport"));
    // Contiguous beats gapped.
    REQUIRE(fuzzy_score("open", "Overview Panel Editor Node"));
    CHECK(*fuzzy_score("open", "Open glTF...") > *fuzzy_score("open", "Overview Panel Editor Node"));
}

TEST_CASE("fuzzy: ranks the expected command first") {
    CHECK(top("reset") == "Reset Layout");
    CHECK(top("rl") == "Reset Layout");
    CHECK(top("tc") == "Toggle Console");
    CHECK(top("tab") == "Toggle Asset Browser");
    CHECK(top("pal") == "Command Palette");
    CHECK(top("pref") == "Preferences");
    CHECK(top("ortho") == "Toggle Orthographic");
    CHECK(top("vt") == "View Top");
    CHECK(top("frame") == "Frame Selection");
    CHECK(top("q") == "Quit");
    CHECK(top("zzz") == "<none>");
    // Every command matches the empty pattern; ties go to the shorter name.
    CHECK(fuzzy_rank("", kCommands).size() == kCommands.size());
    CHECK(top("") == "Quit");
}
