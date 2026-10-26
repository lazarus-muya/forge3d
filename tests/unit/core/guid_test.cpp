// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/guid.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

using f3d::core::Guid;

TEST_CASE("Guid: default is null") {
    Guid g;
    CHECK(g.is_null());
    CHECK(g == Guid{});
    CHECK(g.to_string() == "00000000-0000-0000-0000-000000000000");
}

TEST_CASE("Guid: generate produces 1M unique version-4 values") {
    constexpr std::size_t count = 1'000'000;
    std::vector<Guid> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(Guid::generate());
    }
    for (std::size_t i = 0; i < 1000; ++i) {
        const Guid& g = ids[i];
        REQUIRE_FALSE(g.is_null());
        REQUIRE(((g.high() >> 12) & 0xF) == 4); // version nibble
        REQUIRE((g.low() >> 62) == 0b10);       // RFC 4122 variant
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("Guid: to_string / parse round-trip") {
    for (int i = 0; i < 1000; ++i) {
        const Guid g = Guid::generate();
        const std::string s = g.to_string();
        REQUIRE(s.size() == 36);
        REQUIRE(s[8] == '-');
        REQUIRE(s[13] == '-');
        REQUIRE(s[14] == '4');
        REQUIRE(s[18] == '-');
        REQUIRE(s[23] == '-');
        const auto parsed = Guid::parse(s);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == g);
    }
}

TEST_CASE("Guid: known value formats and parses") {
    const Guid g(0x0123456789abcdefull, 0xfedcba9876543210ull);
    CHECK(g.to_string() == "01234567-89ab-cdef-fedc-ba9876543210");
    CHECK(Guid::parse("01234567-89AB-CDEF-FEDC-BA9876543210") == g); // upper case accepted
}

TEST_CASE("Guid: parse rejects malformed input") {
    CHECK_FALSE(Guid::parse("").has_value());
    CHECK_FALSE(Guid::parse("01234567-89ab-cdef-fedc-ba987654321").has_value());   // too short
    CHECK_FALSE(Guid::parse("01234567-89ab-cdef-fedc-ba98765432100").has_value()); // too long
    CHECK_FALSE(Guid::parse("01234567089ab-cdef-fedc-ba9876543210").has_value());  // missing dash
    CHECK_FALSE(Guid::parse("0123456g-89ab-cdef-fedc-ba9876543210").has_value());  // bad digit
    CHECK_FALSE(Guid::parse("{1234567-89ab-cdef-fedc-ba9876543210").has_value());
}

TEST_CASE("Guid: ordering and hashing") {
    CHECK(Guid(0, 1) < Guid(0, 2));
    CHECK(Guid(0, ~0ull) < Guid(1, 0));

    const std::hash<Guid> hasher;
    const Guid a = Guid::generate();
    CHECK(hasher(a) == hasher(*Guid::parse(a.to_string())));

    // Sequential values (as used in fixtures) must still hash well.
    std::unordered_set<std::size_t> hashes;
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        hashes.insert(hasher(Guid(0, i)));
    }
    CHECK(hashes.size() == 100'000);

    std::unordered_set<Guid> set;
    for (int i = 0; i < 10'000; ++i) {
        set.insert(Guid::generate());
    }
    CHECK(set.size() == 10'000);
}
