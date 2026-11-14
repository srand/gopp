// The unicode/utf8 package: runes to and from UTF-8, ported from Go
// 1.27.3.
//
//     auto [r, size] = utf8::DecodeRuneInString("héllo");   // 'h', 1
//     utf8::RuneCountInString("héllo");                     // 5
//     b = utf8::AppendRune(b, U'世');
//
// Each function has Go's two forms: std::span<const byte> for []byte and
// std::string_view for string (the ...InString functions).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/slice.hpp>

#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#endif

namespace gopp::utf8 {

inline constexpr rune RuneError = 0xFFFD;  // the "error" rune, U+FFFD
inline constexpr rune RuneSelf = 0x80;     // runes below it are a single byte
inline constexpr rune MaxRune = 0x10FFFF;  // the largest valid code point
inline constexpr int UTFMax = 4;           // the most bytes in one rune's encoding

namespace detail {

inline constexpr rune surrogateMin = 0xD800;
inline constexpr rune surrogateMax = 0xDFFF;

inline constexpr byte tx = 0b10000000;
inline constexpr byte t2 = 0b11000000;
inline constexpr byte t3 = 0b11100000;
inline constexpr byte t4 = 0b11110000;
inline constexpr byte maskx = 0b00111111;
inline constexpr byte mask2 = 0b00011111;
inline constexpr byte mask3 = 0b00001111;
inline constexpr byte mask4 = 0b00000111;

inline constexpr rune rune1Max = (1 << 7) - 1;
inline constexpr rune rune2Max = (1 << 11) - 1;
inline constexpr rune rune3Max = (1 << 16) - 1;

// The default lowest and highest continuation byte.
inline constexpr byte locb = 0b10000000;
inline constexpr byte hicb = 0b10111111;

// The names of the first table's entries: the high nibble is an index
// into acceptRanges or F for special one-byte cases; the low nibble is
// the size of the sequence (or, for xx, 1).
inline constexpr byte xx = 0xF1;  // invalid: size 1
inline constexpr byte as = 0xF0;  // ASCII: size 1
inline constexpr byte s1 = 0x02;  // accept 0, size 2
inline constexpr byte s2 = 0x13;  // accept 1, size 3
inline constexpr byte s3 = 0x03;  // accept 0, size 3
inline constexpr byte s4 = 0x23;  // accept 2, size 3
inline constexpr byte s5 = 0x34;  // accept 3, size 4
inline constexpr byte s6 = 0x04;  // accept 0, size 4
inline constexpr byte s7 = 0x44;  // accept 4, size 4

inline constexpr byte runeErrorByte0 = t3 | (RuneError >> 12);
inline constexpr byte runeErrorByte1 = tx | ((RuneError >> 6) & maskx);
inline constexpr byte runeErrorByte2 = tx | (RuneError & maskx);

// Information about the first byte of a UTF-8 sequence.
inline constexpr std::array<byte, 256> first = [] {
    std::array<byte, 256> t{};
    for (int i = 0x00; i < 0x80; i++) t[i] = as;
    for (int i = 0x80; i < 0xC2; i++) t[i] = xx;
    for (int i = 0xC2; i < 0xE0; i++) t[i] = s1;
    t[0xE0] = s2;
    for (int i = 0xE1; i < 0xF0; i++) t[i] = s3;
    t[0xED] = s4;
    t[0xF0] = s5;
    t[0xF1] = t[0xF2] = t[0xF3] = s6;
    t[0xF4] = s7;
    for (int i = 0xF5; i < 0x100; i++) t[i] = xx;
    return t;
}();

// The valid range of the second byte of a sequence.
struct acceptRange {
    byte lo;  // lowest value for second byte.
    byte hi;  // highest value for second byte.
};

inline constexpr std::array<acceptRange, 16> acceptRanges = {{
    {locb, hicb},
    {0xA0, hicb},
    {locb, 0x9F},
    {0x90, hicb},
    {locb, 0x8F},
}};

inline constexpr bool fullRune(const byte* p, std::size_t n) noexcept {
    if (n == 0) return false;
    byte x = first[p[0]];
    if (n >= std::size_t(x & 7)) return true;  // ASCII, invalid or valid.
    // Must be short or invalid.
    auto accept = acceptRanges[x >> 4];
    if (n > 1 && (p[1] < accept.lo || accept.hi < p[1])) return true;
    if (n > 2 && (p[2] < locb || hicb < p[2])) return true;
    return false;
}

inline constexpr std::pair<rune, int> decodeRune(const byte* p, std::size_t n) noexcept {
    if (n < 1) return {RuneError, 0};
    byte p0 = p[0];
    byte x = first[p0];
    if (x >= as) {
        // The following code simulates an additional check for x == xx and
        // handling the ASCII and invalid cases accordingly.
        if (x == xx) return {RuneError, 1};
        return {rune(p0), 1};
    }
    std::size_t sz = x & 7;
    auto accept = acceptRanges[x >> 4];
    if (n < sz) return {RuneError, 1};
    byte b1 = p[1];
    if (b1 < accept.lo || accept.hi < b1) return {RuneError, 1};
    if (sz <= 2) return {rune(p0 & mask2) << 6 | rune(b1 & maskx), 2};
    byte b2 = p[2];
    if (b2 < locb || hicb < b2) return {RuneError, 1};
    if (sz <= 3) return {rune(p0 & mask3) << 12 | rune(b1 & maskx) << 6 | rune(b2 & maskx), 3};
    byte b3 = p[3];
    if (b3 < locb || hicb < b3) return {RuneError, 1};
    return {rune(p0 & mask4) << 18 | rune(b1 & maskx) << 12 | rune(b2 & maskx) << 6 | rune(b3 & maskx), 4};
}

inline constexpr bool runeStart(byte b) noexcept { return (b & 0xC0) != 0x80; }

inline constexpr std::pair<rune, int> decodeLastRune(const byte* p, std::size_t n) noexcept {
    if (n == 0) return {RuneError, 0};
    std::ptrdiff_t end = std::ptrdiff_t(n);
    std::ptrdiff_t start = end - 1;
    rune r = p[start];
    if (r < RuneSelf) return {r, 1};
    // Guard against O(n^2) behavior when traversing backwards through
    // strings with long sequences of invalid UTF-8.
    std::ptrdiff_t lim = end - UTFMax > 0 ? end - UTFMax : 0;
    for (start--; start >= lim; start--) {
        if (runeStart(p[start])) break;
    }
    if (start < 0) start = 0;
    auto [r2, size] = decodeRune(p + start, std::size_t(end - start));
    if (start + size != end) return {RuneError, 1};
    return {r2, size};
}

inline constexpr int runeCount(const byte* p, std::size_t n) noexcept {
    int count = 0;
    std::size_t i = 0;
    while (i < n) {
        if (p[i] < RuneSelf) {
            i++;
        } else {
            i += std::size_t(decodeRune(p + i, n - i).second);
        }
        count++;
    }
    return count;
}

inline constexpr bool valid(const byte* p, std::size_t n) noexcept {
    std::size_t i = 0;
    while (i < n) {
        byte p0 = p[i];
        if (p0 < RuneSelf) {
            i++;
            continue;
        }
        byte x = first[p0];
        std::size_t size = x & 7;
        auto accept = acceptRanges[x >> 4];
        std::size_t left = n - i;
        switch (size) {
        case 2:
            if (left < 2 || p[i + 1] < accept.lo || accept.hi < p[i + 1]) return false;
            break;
        case 3:
            if (left < 3 || p[i + 1] < accept.lo || accept.hi < p[i + 1] || p[i + 2] < locb || hicb < p[i + 2])
                return false;
            break;
        case 4:
            if (left < 4 || p[i + 1] < accept.lo || accept.hi < p[i + 1] || p[i + 2] < locb || hicb < p[i + 2] ||
                p[i + 3] < locb || hicb < p[i + 3])
                return false;
            break;
        default:
            return false;  // illegal starter byte
        }
        i += size;
    }
    return true;
}

// The encoding of r into p, which has room for it; returns its length.
inline constexpr int encodeRune(byte* p, rune r) noexcept {
    auto i = static_cast<uint32>(r);
    if (i <= uint32(rune1Max)) {
        p[0] = byte(r);
        return 1;
    }
    if (i <= uint32(rune2Max)) {
        p[0] = t2 | byte(r >> 6);
        p[1] = tx | (byte(r) & maskx);
        return 2;
    }
    if (i < uint32(surrogateMin) || (uint32(surrogateMax) < i && i <= uint32(rune3Max))) {
        p[0] = t3 | byte(r >> 12);
        p[1] = tx | (byte(r >> 6) & maskx);
        p[2] = tx | (byte(r) & maskx);
        return 3;
    }
    if (i > uint32(rune3Max) && i <= uint32(MaxRune)) {
        p[0] = t4 | byte(r >> 18);
        p[1] = tx | (byte(r >> 12) & maskx);
        p[2] = tx | (byte(r >> 6) & maskx);
        p[3] = tx | (byte(r) & maskx);
        return 4;
    }
    p[0] = runeErrorByte0;
    p[1] = runeErrorByte1;
    p[2] = runeErrorByte2;
    return 3;
}

inline const byte* bytes_of(std::string_view s) noexcept { return reinterpret_cast<const byte*>(s.data()); }

}  // namespace detail

// utf8.FullRune: whether p begins with a full UTF-8 encoding of a rune. An
// invalid encoding counts as a full rune, as it decodes to a width-1
// RuneError.
constexpr bool FullRune(std::span<const byte> p) noexcept { return detail::fullRune(p.data(), p.size()); }
// utf8.FullRuneInString
inline bool FullRuneInString(std::string_view s) noexcept { return detail::fullRune(detail::bytes_of(s), s.size()); }

// utf8.DecodeRune: the first rune of p and its width in bytes. RuneError
// and 1 for an invalid encoding, RuneError and 0 for an empty p.
constexpr std::pair<rune, int> DecodeRune(std::span<const byte> p) noexcept {
    if (!p.empty() && p[0] < RuneSelf) return {rune(p[0]), 1};
    return detail::decodeRune(p.data(), p.size());
}
// utf8.DecodeRuneInString
inline std::pair<rune, int> DecodeRuneInString(std::string_view s) noexcept {
    if (!s.empty() && static_cast<byte>(s[0]) < RuneSelf) return {rune(static_cast<byte>(s[0])), 1};
    return detail::decodeRune(detail::bytes_of(s), s.size());
}

// utf8.DecodeLastRune: as DecodeRune for the last rune of p.
constexpr std::pair<rune, int> DecodeLastRune(std::span<const byte> p) noexcept {
    return detail::decodeLastRune(p.data(), p.size());
}
// utf8.DecodeLastRuneInString
inline std::pair<rune, int> DecodeLastRuneInString(std::string_view s) noexcept {
    return detail::decodeLastRune(detail::bytes_of(s), s.size());
}

// utf8.RuneLen: the number of bytes of r's encoding; -1 if r isn't a
// valid rune.
constexpr int RuneLen(rune r) noexcept {
    if (r < 0) return -1;
    if (r <= detail::rune1Max) return 1;
    if (r <= detail::rune2Max) return 2;
    if (detail::surrogateMin <= r && r <= detail::surrogateMax) return -1;
    if (r <= detail::rune3Max) return 3;
    if (r <= MaxRune) return 4;
    return -1;
}

// utf8.EncodeRune: writes the encoding of r (RuneError's if r isn't a
// valid rune) into p and returns its length. Panics if p is too small.
inline int EncodeRune(std::span<byte> p, rune r) {
    byte buf[UTFMax];
    int n = detail::encodeRune(buf, r);
    if (p.size() < std::size_t(n)) panic("index out of range");
    std::memcpy(p.data(), buf, std::size_t(n));
    return n;
}

// utf8.AppendRune: p with the encoding of r appended.
inline slice<byte> AppendRune(slice<byte> p, rune r) {
    byte buf[UTFMax];
    int n = detail::encodeRune(buf, r);
    return appendSpan(std::move(p), std::span<const byte>(buf, std::size_t(n)));
}

// utf8.RuneCount: the number of runes in p; each invalid byte counts as one.
constexpr int RuneCount(std::span<const byte> p) noexcept { return detail::runeCount(p.data(), p.size()); }
// utf8.RuneCountInString
inline int RuneCountInString(std::string_view s) noexcept { return detail::runeCount(detail::bytes_of(s), s.size()); }

// utf8.RuneStart: whether b could be the first byte of an encoded rune.
constexpr bool RuneStart(byte b) noexcept { return detail::runeStart(b); }

// utf8.Valid: whether p is entirely valid UTF-8.
constexpr bool Valid(std::span<const byte> p) noexcept { return detail::valid(p.data(), p.size()); }
// utf8.ValidString
inline bool ValidString(std::string_view s) noexcept { return detail::valid(detail::bytes_of(s), s.size()); }

// utf8.ValidRune: whether r is a code point that UTF-8 can encode:
// surrogates and values past MaxRune aren't.
constexpr bool ValidRune(rune r) noexcept {
    if (0 <= r && r < detail::surrogateMin) return true;
    if (detail::surrogateMax < r && r <= MaxRune) return true;
    return false;
}

}  // namespace gopp::utf8
