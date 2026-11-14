// Go's net/http/internal/ascii: ASCII-only case folding for header names,
// tokens and methods, where Unicode folding (strings::EqualFold) would
// let "ſ" match "s".
#pragma once

#include <gopp/string.hpp>

#include <string_view>
#include <utility>

namespace gopp::http::ascii {

constexpr char lower(char b) noexcept { return 'A' <= b && b <= 'Z' ? char(b + ('a' - 'A')) : b; }

// ascii.EqualFold: whether s and t are equal under ASCII case folding.
constexpr bool EqualFold(std::string_view s, std::string_view t) noexcept {
    if (s.size() != t.size()) return false;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (lower(s[i]) != lower(t[i])) return false;
    }
    return true;
}

// ascii.IsPrint: whether s is printable ASCII.
constexpr bool IsPrint(std::string_view s) noexcept {
    for (char c : s) {
        if (c < ' ' || c > '~') return false;
    }
    return true;
}

// s with its ASCII letters lowered and other bytes kept.
inline string ToLowerBytes(std::string_view s) {
    return string::build(s.size(), [&](char* p) {
        for (std::size_t i = 0; i < s.size(); i++) p[i] = lower(s[i]);
    });
}

}  // namespace gopp::http::ascii
