// Ported from Go 1.27.3's bytes/bytes.go and strings/strings.go.
#include "text.hpp"

#include <gopp/unicode/utf8.hpp>

#include <array>
#include <cstring>
#include <limits>

namespace gopp::strings::text {

namespace {

using utf8::RuneError;
using utf8::RuneSelf;

constexpr isize nposToIsize(std::size_t i) noexcept { return i == sv::npos ? -1 : static_cast<isize>(i); }

byte at(sv s, std::size_t i) noexcept { return static_cast<byte>(s[i]); }

// asciiSet: a 256-bit set of ASCII bytes; makeASCIISet fails (returns
// nullopt) if chars has a non-ASCII byte.
using asciiSet = std::array<bool, 256>;

std::optional<asciiSet> makeASCIISet(sv chars) noexcept {
    asciiSet as{};
    for (unsigned char c : chars) {
        if (c >= RuneSelf) return std::nullopt;
        as[c] = true;
    }
    return as;
}

bool shouldUseASCIISet(std::size_t bufLen) noexcept { return bufLen > 8; }

// Whether the string contains the rune, decoding as Go's range loop.
bool containsRune(sv s, rune r) noexcept {
    while (!s.empty()) {
        auto [c, n] = utf8::DecodeRuneInString(s);
        if (c == r) return true;
        s.remove_prefix(std::size_t(n));
    }
    return false;
}

// The encoding of r, as Go's string(r).
std::string encode(rune r) {
    byte buf[utf8::UTFMax];
    int n = utf8::EncodeRune(buf, r);
    return std::string(reinterpret_cast<const char*>(buf), std::size_t(n));
}

sv trimLeftByte(sv s, char c) noexcept {
    while (!s.empty() && s[0] == c) s.remove_prefix(1);
    if (s.empty()) return nil;
    return s;
}

sv trimLeftASCII(sv s, const asciiSet& as) noexcept {
    while (!s.empty()) {
        if (!as[at(s, 0)]) break;
        s.remove_prefix(1);
    }
    if (s.empty()) return nil;
    return s;
}

sv trimLeftUnicode(sv s, sv cutset) noexcept {
    while (!s.empty()) {
        auto [r, n] = utf8::DecodeRuneInString(s);
        if (!containsRune(cutset, r)) break;
        s.remove_prefix(std::size_t(n));
    }
    if (s.empty()) return nil;
    return s;
}

sv trimRightByte(sv s, char c) noexcept {
    while (!s.empty() && s.back() == c) s.remove_suffix(1);
    return s;
}

sv trimRightASCII(sv s, const asciiSet& as) noexcept {
    while (!s.empty()) {
        if (!as[at(s, s.size() - 1)]) break;
        s.remove_suffix(1);
    }
    return s;
}

sv trimRightUnicode(sv s, sv cutset) noexcept {
    while (!s.empty()) {
        rune r = at(s, s.size() - 1);
        int n = 1;
        if (r >= RuneSelf) std::tie(r, n) = utf8::DecodeLastRuneInString(s);
        if (!containsRune(cutset, r)) break;
        s.remove_suffix(std::size_t(n));
    }
    return s;
}

// The parts of s for an empty separator: its UTF-8 sequences, at most n
// of them (n <= 0 means no limit); the last holds the rest of s.
std::vector<sv> explode(sv s, isize n) {
    if (n <= 0 || std::size_t(n) > s.size()) n = isize(s.size());
    std::vector<sv> a;
    a.reserve(std::size_t(n));
    while (!s.empty()) {
        if (isize(a.size()) + 1 >= n) {
            a.push_back(s);
            break;
        }
        auto [_, size] = utf8::DecodeRuneInString(s);
        a.push_back(s.substr(0, std::size_t(size)));
        s.remove_prefix(std::size_t(size));
    }
    return a;
}

}  // namespace

isize index(sv s, sv sep) noexcept { return nposToIsize(s.find(sep)); }

isize lastIndex(sv s, sv sep) noexcept {
    if (sep.empty()) return isize(s.size());
    return nposToIsize(s.rfind(sep));
}

isize indexByte(sv s, char c) noexcept { return nposToIsize(s.find(c)); }

isize lastIndexByte(sv s, char c) noexcept { return nposToIsize(s.rfind(c)); }

isize indexRune(sv s, rune r) noexcept {
    if (0 <= r && r < RuneSelf) return indexByte(s, char(r));
    if (r == RuneError) {
        for (std::size_t i = 0; i < s.size();) {
            auto [r1, n] = utf8::DecodeRuneInString(s.substr(i));
            if (r1 == RuneError) return isize(i);
            i += std::size_t(n);
        }
        return -1;
    }
    if (!utf8::ValidRune(r)) return -1;
    byte b[utf8::UTFMax];
    int n = utf8::EncodeRune(b, r);
    return index(s, sv(reinterpret_cast<const char*>(b), std::size_t(n)));
}

isize indexAny(sv s, sv chars) noexcept {
    if (chars.empty()) {
        // Avoid scanning all of s.
        return -1;
    }
    if (s.size() == 1) {
        rune r = at(s, 0);
        if (r >= RuneSelf) {
            // search utf8.RuneError.
            return containsRune(chars, RuneError) ? 0 : -1;
        }
        return chars.find(s[0]) != sv::npos ? 0 : -1;
    }
    if (chars.size() == 1) {
        rune r = static_cast<byte>(chars[0]);
        if (r >= RuneSelf) r = RuneError;
        return indexRune(s, r);
    }
    if (shouldUseASCIISet(s.size())) {
        if (auto as = makeASCIISet(chars)) {
            for (std::size_t i = 0; i < s.size(); i++) {
                if ((*as)[at(s, i)]) return isize(i);
            }
            return -1;
        }
    }
    int width = 0;
    for (std::size_t i = 0; i < s.size(); i += std::size_t(width)) {
        rune r = at(s, i);
        if (r < RuneSelf) {
            if (chars.find(s[i]) != sv::npos) return isize(i);
            width = 1;
            continue;
        }
        std::tie(r, width) = utf8::DecodeRuneInString(s.substr(i));
        if (r != RuneError) {
            // r is 2 to 4 bytes
            if (chars.size() == std::size_t(width)) {
                if (chars == encode(r)) return isize(i);
                continue;
            }
            if (chars.find(encode(r)) != sv::npos) return isize(i);
            continue;
        }
        if (containsRune(chars, r)) return isize(i);
    }
    return -1;
}

isize lastIndexAny(sv s, sv chars) noexcept {
    if (chars.empty()) {
        // Avoid scanning all of s.
        return -1;
    }
    if (shouldUseASCIISet(s.size())) {
        if (auto as = makeASCIISet(chars)) {
            for (std::size_t i = s.size(); i-- > 0;) {
                if ((*as)[at(s, i)]) return isize(i);
            }
            return -1;
        }
    }
    if (s.size() == 1) {
        rune r = at(s, 0);
        if (r >= RuneSelf) return containsRune(chars, RuneError) ? 0 : -1;
        return chars.find(s[0]) != sv::npos ? 0 : -1;
    }
    if (chars.size() == 1) {
        rune cr = static_cast<byte>(chars[0]);
        if (cr >= RuneSelf) cr = RuneError;
        for (std::size_t i = s.size(); i > 0;) {
            auto [r, size] = utf8::DecodeLastRuneInString(s.substr(0, i));
            i -= std::size_t(size);
            if (r == cr) return isize(i);
        }
        return -1;
    }
    for (std::size_t i = s.size(); i > 0;) {
        rune r = at(s, i - 1);
        if (r < RuneSelf) {
            if (chars.find(s[i - 1]) != sv::npos) return isize(i - 1);
            i--;
            continue;
        }
        auto [r2, size] = utf8::DecodeLastRuneInString(s.substr(0, i));
        i -= std::size_t(size);
        if (r2 != RuneError) {
            // r is 2 to 4 bytes
            if (chars.size() == std::size_t(size)) {
                if (chars == encode(r2)) return isize(i);
                continue;
            }
            if (chars.find(encode(r2)) != sv::npos) return isize(i);
            continue;
        }
        if (containsRune(chars, r2)) return isize(i);
    }
    return -1;
}

isize indexFunc(sv s, pred f, bool truth) {
    for (std::size_t start = 0; start < s.size();) {
        auto [r, wid] = utf8::DecodeRuneInString(s.substr(start));
        if (f(r) == truth) return isize(start);
        start += std::size_t(wid);
    }
    return -1;
}

isize lastIndexFunc(sv s, pred f, bool truth) {
    for (std::size_t i = s.size(); i > 0;) {
        rune r = at(s, i - 1);
        int size = 1;
        if (r >= RuneSelf) std::tie(r, size) = utf8::DecodeLastRuneInString(s.substr(0, i));
        i -= std::size_t(size);
        if (f(r) == truth) return isize(i);
    }
    return -1;
}

isize count(sv s, sv sep) noexcept {
    // special case
    if (sep.empty()) return utf8::RuneCountInString(s) + 1;
    if (sep.size() == 1) return std::count(s.begin(), s.end(), sep[0]);
    isize n = 0;
    for (;;) {
        auto i = s.find(sep);
        if (i == sv::npos) return n;
        n++;
        s.remove_prefix(i + sep.size());
    }
}

bool equal_fold(sv s, sv t) noexcept {
    // ASCII fast path
    std::size_t i = 0;
    for (std::size_t n = std::min(s.size(), t.size()); i < n; i++) {
        byte sr = at(s, i), tr = at(t, i);
        if ((sr | tr) >= RuneSelf) goto hasUnicode;
        // Easy case.
        if (tr == sr) continue;
        // Make sr < tr to simplify what follows.
        if (tr < sr) std::swap(tr, sr);
        // ASCII only, sr/tr must be upper/lower case
        if ('A' <= sr && sr <= 'Z' && tr == sr + 'a' - 'A') continue;
        return false;
    }
    // Check if we've exhausted both strings.
    return s.size() == t.size();

hasUnicode:
    s.remove_prefix(i);
    t.remove_prefix(i);
    while (!s.empty() && !t.empty()) {
        // Extract first rune from each string.
        auto [sr, ssize] = utf8::DecodeRuneInString(s);
        s.remove_prefix(std::size_t(ssize));
        auto [tr, tsize] = utf8::DecodeRuneInString(t);
        t.remove_prefix(std::size_t(tsize));

        // If they match, keep going; if not, return false.

        // Easy case.
        if (tr == sr) continue;
        // Make sr < tr to simplify what follows.
        if (tr < sr) std::swap(tr, sr);
        // Fast check for ASCII.
        if (tr < RuneSelf) {
            // ASCII only, sr/tr must be upper/lower case
            if ('A' <= sr && sr <= 'Z' && tr == sr + 'a' - 'A') continue;
            return false;
        }
        // General case. SimpleFold(x) returns the next equivalent rune > x
        // or wraps around to smaller values.
        rune r = unicode::SimpleFold(sr);
        while (r != sr && r < tr) r = unicode::SimpleFold(r);
        if (r == tr) continue;
        return false;
    }
    // One string is empty. Are both?
    return s.size() == t.size();
}

int compare(sv a, sv b) noexcept {
    int c = a.compare(b);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

sv trim(sv s, sv cutset) noexcept {
    if (s.empty()) {
        // This is what we've historically done.
        return nil;
    }
    if (cutset.empty()) return s;
    if (cutset.size() == 1 && static_cast<byte>(cutset[0]) < RuneSelf) {
        return trimLeftByte(trimRightByte(s, cutset[0]), cutset[0]);
    }
    if (auto as = makeASCIISet(cutset)) return trimLeftASCII(trimRightASCII(s, *as), *as);
    return trimLeftUnicode(trimRightUnicode(s, cutset), cutset);
}

sv trimLeft(sv s, sv cutset) noexcept {
    if (s.empty()) {
        // This is what we've historically done.
        return nil;
    }
    if (cutset.empty()) return s;
    if (cutset.size() == 1 && static_cast<byte>(cutset[0]) < RuneSelf) return trimLeftByte(s, cutset[0]);
    if (auto as = makeASCIISet(cutset)) return trimLeftASCII(s, *as);
    return trimLeftUnicode(s, cutset);
}

sv trimRight(sv s, sv cutset) noexcept {
    if (s.empty() || cutset.empty()) return s;
    if (cutset.size() == 1 && static_cast<byte>(cutset[0]) < RuneSelf) return trimRightByte(s, cutset[0]);
    if (auto as = makeASCIISet(cutset)) return trimRightASCII(s, *as);
    return trimRightUnicode(s, cutset);
}

sv trim_space(sv s) noexcept {
    // Fast path for ASCII: look for the first ASCII non-space byte.
    for (std::size_t lo = 0; lo < s.size(); lo++) {
        byte c = at(s, lo);
        if (c >= RuneSelf) {
            // If we run into a non-ASCII byte, fall back to the slower
            // unicode-aware method on the remaining bytes.
            return trimFunc(s.substr(lo), unicode::IsSpace);
        }
        if (asciiSpace(c)) continue;
        s.remove_prefix(lo);
        // Now look for the first ASCII non-space byte from the end.
        for (std::size_t hi = s.size(); hi-- > 0;) {
            byte c2 = at(s, hi);
            if (c2 >= RuneSelf) return trimFunc(s.substr(0, hi + 1), unicode::IsSpace);
            if (!asciiSpace(c2)) {
                // At this point, s[:hi+1] starts and ends with ASCII
                // non-space bytes, so we're done. Non-ASCII cases have
                // already been handled above.
                return s.substr(0, hi + 1);
            }
        }
    }
    // Special case to preserve previous TrimLeftFunc behavior, returning
    // nil instead of empty slice if all spaces.
    return nil;
}

sv trimLeftFunc(sv s, pred f) {
    isize i = indexFunc(s, f, false);
    if (i == -1) return nil;
    return s.substr(std::size_t(i));
}

sv trimRightFunc(sv s, pred f) {
    isize i = lastIndexFunc(s, f, false);
    if (i >= 0 && at(s, std::size_t(i)) >= RuneSelf) {
        auto [_, wid] = utf8::DecodeRuneInString(s.substr(std::size_t(i)));
        i += wid;
    } else {
        i++;
    }
    return s.substr(0, std::size_t(i));
}

sv trimFunc(sv s, pred f) { return trimRightFunc(trimLeftFunc(s, f), f); }

cut_result cut(sv s, sv sep) noexcept {
    if (auto i = s.find(sep); i != sv::npos) return {s.substr(0, i), s.substr(i + sep.size()), true};
    return {s, nil, false};
}

cut_result cutLast(sv s, sv sep) noexcept {
    if (auto i = lastIndex(s, sep); i >= 0) return {s.substr(0, std::size_t(i)), s.substr(std::size_t(i) + sep.size()), true};
    return {s, nil, false};
}

std::vector<sv> genSplit(sv s, sv sep, std::size_t sep_save, isize n) {
    if (sep.empty()) return explode(s, n);
    if (n < 0) n = count(s, sep) + 1;
    n = std::min(n, isize(s.size()) + 1);
    std::vector<sv> a;
    a.reserve(std::size_t(n));
    n--;
    while (isize(a.size()) < n) {
        auto m = s.find(sep);
        if (m == sv::npos) break;
        a.push_back(s.substr(0, m + sep_save));
        s.remove_prefix(m + sep.size());
    }
    a.push_back(s);
    return a;
}

std::vector<sv> fields(sv s) {
    // First count the fields. This is an exact count if s is ASCII,
    // otherwise it is an approximation.
    std::size_t n = 0;
    bool wasSpace = true;
    // setBits is used to track which bits are set in the bytes of s.
    byte setBits = 0;
    for (std::size_t i = 0; i < s.size(); i++) {
        byte r = at(s, i);
        setBits |= r;
        bool isSpace = asciiSpace(r);
        n += wasSpace && !isSpace;
        wasSpace = isSpace;
    }
    if (setBits >= RuneSelf) {
        // Some runes in the input slice are not ASCII.
        return fieldsFunc(s, unicode::IsSpace);
    }
    // ASCII fast path
    std::vector<sv> a;
    a.reserve(n);
    std::size_t i = 0;
    // Skip spaces in the front of the input.
    while (i < s.size() && asciiSpace(at(s, i))) i++;
    std::size_t fieldStart = i;
    while (i < s.size()) {
        if (!asciiSpace(at(s, i))) {
            i++;
            continue;
        }
        a.push_back(s.substr(fieldStart, i - fieldStart));
        i++;
        // Skip spaces in between fields.
        while (i < s.size() && asciiSpace(at(s, i))) i++;
        fieldStart = i;
    }
    if (fieldStart < s.size()) {
        // Last field might end at EOF.
        a.push_back(s.substr(fieldStart));
    }
    return a;
}

std::vector<sv> fieldsFunc(sv s, pred f) {
    std::vector<sv> a;
    isize start = -1;  // valid span start if >= 0
    for (std::size_t i = 0; i < s.size();) {
        auto [r, size] = utf8::DecodeRuneInString(s.substr(i));
        if (f(r)) {
            if (start >= 0) {
                a.push_back(s.substr(std::size_t(start), i - std::size_t(start)));
                start = -1;
            }
        } else if (start < 0) {
            start = isize(i);
        }
        i += std::size_t(size);
    }
    // Last field might end at EOF.
    if (start >= 0) a.push_back(s.substr(std::size_t(start)));
    return a;
}

std::string map(mapping f, sv s) {
    std::string b;
    b.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        auto [r, wid] = utf8::DecodeRuneInString(s.substr(i));
        r = f(r);
        if (r >= 0) {
            byte buf[utf8::UTFMax];
            int n = utf8::EncodeRune(buf, r);
            b.append(reinterpret_cast<const char*>(buf), std::size_t(n));
        }
        i += std::size_t(wid);
    }
    return b;
}

std::optional<std::string> toUpper(sv s) {
    bool isASCII = true, hasLower = false;
    for (unsigned char c : s) {
        if (c >= RuneSelf) {
            isASCII = false;
            break;
        }
        hasLower = hasLower || ('a' <= c && c <= 'z');
    }
    if (isASCII) {
        // optimize for ASCII-only strings.
        if (!hasLower) return std::nullopt;
        std::string b(s);
        for (char& c : b) {
            if ('a' <= c && c <= 'z') c -= 'a' - 'A';
        }
        return b;
    }
    return map(unicode::ToUpper, s);
}

std::optional<std::string> toLower(sv s) {
    bool isASCII = true, hasUpper = false;
    for (unsigned char c : s) {
        if (c >= RuneSelf) {
            isASCII = false;
            break;
        }
        hasUpper = hasUpper || ('A' <= c && c <= 'Z');
    }
    if (isASCII) {
        // optimize for ASCII-only strings.
        if (!hasUpper) return std::nullopt;
        std::string b(s);
        for (char& c : b) {
            if ('A' <= c && c <= 'Z') c += 'a' - 'A';
        }
        return b;
    }
    return map(unicode::ToLower, s);
}

std::string repeat(sv s, isize count, const char* pkg) {
    if (count == 0) return {};
    // Since we cannot return an error on overflow, we should panic if the
    // repeat will generate an overflow. See golang.org/issue/16237.
    if (count < 0) panic(std::string(pkg) + ": negative Repeat count");
    std::size_t n = 0;
    if (__builtin_mul_overflow(s.size(), std::size_t(count), &n) || n > std::size_t(std::numeric_limits<isize>::max())) {
        panic(std::string(pkg) + ": Repeat output length overflow");
    }
    if (s.empty()) return {};
    std::string b;
    if (n > b.max_size()) panic("runtime error: makeslice: len out of range");
    b.resize(n);
    std::memcpy(b.data(), s.data(), s.size());
    // Past a certain chunk size it is counterproductive to use larger
    // chunks as the source of the write, as when the source is too large
    // we are basically just thrashing the CPU D-cache.
    constexpr std::size_t chunkLimit = 8 * 1024;
    std::size_t chunkMax = n;
    if (chunkMax > chunkLimit) {
        chunkMax = chunkLimit / s.size() * s.size();
        if (chunkMax == 0) chunkMax = s.size();
    }
    for (std::size_t bp = s.size(); bp < n;) {
        std::size_t chunk = std::min({bp, chunkMax, n - bp});
        std::memcpy(b.data() + bp, b.data(), chunk);
        bp += chunk;
    }
    return b;
}

std::optional<std::string> replace(sv s, sv old, sv new_, isize n) {
    // Compute number of replacements.
    isize m = 0;
    if (n != 0) m = count(s, old);
    if (m == 0) {
        // Just return a copy.
        return std::nullopt;
    }
    if (n < 0 || m < n) n = m;

    // Apply replacements to buffer.
    std::string t;
    t.reserve(s.size() + std::size_t(n) * new_.size());
    std::size_t start = 0;
    if (!old.empty()) {
        for (isize k = 0; k < n; k++) {
            std::size_t j = s.find(old, start);
            t.append(s.substr(start, j - start));
            t.append(new_);
            start = j + old.size();
        }
    } else {
        // len(old) == 0
        t.append(new_);
        for (isize k = 0; k < n - 1; k++) {
            auto [_, wid] = utf8::DecodeRuneInString(s.substr(start));
            std::size_t j = start + std::size_t(wid);
            t.append(s.substr(start, j - start));
            t.append(new_);
            start = j;
        }
    }
    t.append(s.substr(start));
    return t;
}

std::optional<std::string> toValidUtf8(sv s, sv replacement) {
    if (utf8::ValidString(s)) return std::nullopt;
    std::string b;
    b.reserve(s.size() + replacement.size());
    bool invalid = false;  // previous byte was from an invalid UTF-8 sequence
    for (std::size_t i = 0; i < s.size();) {
        byte c = at(s, i);
        if (c < RuneSelf) {
            i++;
            invalid = false;
            b.push_back(char(c));
            continue;
        }
        auto [_, wid] = utf8::DecodeRuneInString(s.substr(i));
        if (wid == 1) {
            i++;
            if (!invalid) {
                invalid = true;
                b.append(replacement);
            }
            continue;
        }
        invalid = false;
        b.append(s.substr(i, std::size_t(wid)));
        i += std::size_t(wid);
    }
    return b;
}

}  // namespace gopp::strings::text
