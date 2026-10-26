// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace f3d::core {

/// 128-bit identifier for entities and assets. New values are random RFC 4122
/// version-4 UUIDs. The default-constructed Guid is the null Guid.
///
/// Text form is 8-4-4-4-12 lowercase hex, e.g. "3f2b8c1e-7d4a-4f0e-9b1c-2a6d8e4f0c11".
/// `high` holds the first 16 hex digits, `low` the last 16.
class Guid {
public:
    constexpr Guid() noexcept = default;
    constexpr Guid(std::uint64_t high, std::uint64_t low) noexcept : high_(high), low_(low) {}

    /// A new random version-4 Guid (thread-safe).
    [[nodiscard]] static Guid generate();

    /// Parses the 8-4-4-4-12 hex form (either case). Returns nullopt on malformed input.
    [[nodiscard]] static std::optional<Guid> parse(std::string_view text) noexcept;

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr bool is_null() const noexcept { return high_ == 0 && low_ == 0; }
    [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }
    [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }

    friend constexpr bool operator==(const Guid&, const Guid&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Guid&, const Guid&) noexcept = default;

private:
    std::uint64_t high_ = 0;
    std::uint64_t low_ = 0;
};

} // namespace f3d::core

template <>
struct std::hash<f3d::core::Guid> {
    std::size_t operator()(const f3d::core::Guid& g) const noexcept {
        // splitmix-style mix of both halves; GUIDs are random but tests and
        // fixtures may use sequential values.
        std::uint64_t x = g.high() ^ (g.low() + 0x9e3779b97f4a7c15ull + (g.high() << 6) + (g.high() >> 2));
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return static_cast<std::size_t>(x);
    }
};
