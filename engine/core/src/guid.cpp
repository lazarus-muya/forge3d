// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/guid.hpp"

#include <array>
#include <random>

namespace f3d::core {
namespace {

std::mt19937_64& rng() {
    thread_local std::mt19937_64 engine = [] {
        std::random_device rd;
        std::array<std::uint32_t, 8> seed{};
        for (auto& s : seed) {
            s = rd();
        }
        std::seed_seq seq(seed.begin(), seed.end());
        return std::mt19937_64(seq);
    }();
    return engine;
}

constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

constexpr bool is_dash_position(std::size_t i) noexcept {
    return i == 8 || i == 13 || i == 18 || i == 23;
}

} // namespace

Guid Guid::generate() {
    auto& engine = rng();
    std::uint64_t high = engine();
    std::uint64_t low = engine();
    high = (high & ~0xF000ull) | 0x4000ull;                      // version 4
    low = (low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; // variant 10xx
    return Guid(high, low);
}

std::optional<Guid> Guid::parse(std::string_view text) noexcept {
    if (text.size() != 36) {
        return std::nullopt;
    }
    std::uint64_t halves[2] = {0, 0};
    int digit = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (is_dash_position(i)) {
            if (text[i] != '-') {
                return std::nullopt;
            }
            continue;
        }
        const int v = hex_value(text[i]);
        if (v < 0) {
            return std::nullopt;
        }
        auto& half = halves[digit / 16];
        half = (half << 4) | static_cast<std::uint64_t>(v);
        ++digit;
    }
    return Guid(halves[0], halves[1]);
}

std::string Guid::to_string() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(36, '-');
    int digit = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (is_dash_position(i)) {
            continue;
        }
        const std::uint64_t half = digit < 16 ? high_ : low_;
        const int shift = 60 - 4 * (digit % 16);
        out[i] = digits[(half >> shift) & 0xF];
        ++digit;
    }
    return out;
}

} // namespace f3d::core
