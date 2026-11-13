// The parts of Go's unicode/utf8 that the ported packages use.
#pragma once

#include <gopp/base.hpp>

#include <string_view>
#include <utility>

namespace gopp::utf8 {

inline constexpr rune RuneError = 0xFFFD;

// utf8.DecodeRuneInString: the first rune of s and its width; RuneError
// and 1 for an invalid encoding, RuneError and 0 for an empty s.
inline std::pair<rune, int> DecodeRuneInString(std::string_view s) noexcept {
    if (s.empty()) return {RuneError, 0};
    auto b = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    unsigned char c0 = b(0);
    if (c0 < 0x80) return {c0, 1};
    auto cont = [&](std::size_t i) { return i < s.size() && (b(i) & 0xC0) == 0x80; };
    if (c0 >= 0xC2 && c0 <= 0xDF) {
        if (!cont(1)) return {RuneError, 1};
        return {rune(c0 & 0x1F) << 6 | rune(b(1) & 0x3F), 2};
    }
    if (c0 >= 0xE0 && c0 <= 0xEF) {
        if (!cont(1) || !cont(2)) return {RuneError, 1};
        if (c0 == 0xE0 && b(1) < 0xA0) return {RuneError, 1};  // overlong
        if (c0 == 0xED && b(1) > 0x9F) return {RuneError, 1};  // surrogates
        return {rune(c0 & 0x0F) << 12 | rune(b(1) & 0x3F) << 6 | rune(b(2) & 0x3F), 3};
    }
    if (c0 >= 0xF0 && c0 <= 0xF4) {
        if (!cont(1) || !cont(2) || !cont(3)) return {RuneError, 1};
        if (c0 == 0xF0 && b(1) < 0x90) return {RuneError, 1};  // overlong
        if (c0 == 0xF4 && b(1) > 0x8F) return {RuneError, 1};  // past U+10FFFF
        return {rune(c0 & 0x07) << 18 | rune(b(1) & 0x3F) << 12 | rune(b(2) & 0x3F) << 6 | rune(b(3) & 0x3F), 4};
    }
    return {RuneError, 1};
}

// utf8.ValidString: whether s is valid UTF-8.
inline bool ValidString(std::string_view s) noexcept {
    while (!s.empty()) {
        auto [r, n] = DecodeRuneInString(s);
        if (r == RuneError && n == 1) return false;
        s.remove_prefix(std::size_t(n));
    }
    return true;
}

}  // namespace gopp::utf8
