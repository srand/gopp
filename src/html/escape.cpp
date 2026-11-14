// html, ported from Go 1.27.3's html/escape.go.
#include <gopp/html.hpp>
#include <gopp/unicode/utf8.hpp>

#include "html/entity.hpp"

#include <algorithm>
#include <string>

namespace gopp::html {

namespace {

using namespace detail;

// What the numeric character references 0x80 to 0x9F stand for: Windows-1252.
constexpr rune replacementTable[] = {
    0x20AC,  // first entry is what 0x80 should be replaced with
    0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D,
    0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153,
    0x009D, 0x017E,
    0x0178,  // last entry is 0x9F
};

rune lookup(std::string_view name) {
    auto end = entities + nEntities;
    auto it = std::lower_bound(entities, end, name, [](const entity& e, std::string_view n) { return e.name < n; });
    return it != end && it->name == name ? it->r : 0;
}

const rune* lookup2(std::string_view name) {
    auto end = entities2 + nEntities2;
    auto it = std::lower_bound(entities2, end, name, [](const entity2& e, std::string_view n) { return e.name < n; });
    return it != end && it->name == name ? it->r : nullptr;
}

void appendRune(std::string& b, rune r) {
    byte buf[utf8::UTFMax];
    int n = utf8::EncodeRune(buf, r);
    b.append(reinterpret_cast<const char*>(buf), std::size_t(n));
}

// Unescapes the entity at the start of s (an &) onto b; returns the number
// of bytes of s consumed. Precondition: s[0] == '&'.
std::size_t unescapeEntity(std::string& b, std::string_view s) {
    // http://www.whatwg.org/specs/web-apps/current-work/multipage/tokenization.html,
    // section 8.2.4.69.
    std::size_t i = 1;
    if (s.size() <= 1) {
        b += s[0];
        return 1;
    }

    if (s[i] == '#') {
        if (s.size() <= 3) {  // we need to have at least "&#."
            b += s[0];
            return 1;
        }
        i++;
        char c = s[i];
        bool hex = false;
        if (c == 'x' || c == 'X') {
            hex = true;
            i++;
        }

        uint32 ux = 0;  // Go's rune arithmetic wraps
        while (i < s.size()) {
            c = s[i];
            i++;
            if (hex) {
                if ('0' <= c && c <= '9') {
                    ux = 16 * ux + uint32(c - '0');
                    continue;
                } else if ('a' <= c && c <= 'f') {
                    ux = 16 * ux + uint32(c - 'a' + 10);
                    continue;
                } else if ('A' <= c && c <= 'F') {
                    ux = 16 * ux + uint32(c - 'A' + 10);
                    continue;
                }
            } else if ('0' <= c && c <= '9') {
                ux = 10 * ux + uint32(c - '0');
                continue;
            }
            if (c != ';') i--;
            break;
        }
        if (i <= 3) {  // no characters matched
            b += s[0];
            return 1;
        }

        auto x = rune(ux);
        if (0x80 <= x && x <= 0x9F) {
            // Replace characters from Windows-1252 with UTF-8 equivalents.
            x = replacementTable[x - 0x80];
        } else if (x == 0 || (0xD800 <= x && x <= 0xDFFF) || x > 0x10FFFF || x < 0) {
            // Replace invalid characters with the replacement character.
            x = 0xFFFD;
        }
        appendRune(b, x);
        return i;
    }

    // Consume the maximum number of characters possible, with the consumed
    // characters matching one of the named references.
    while (i < s.size()) {
        char c = s[i];
        i++;
        // Lower-cased characters are more common in entities, so we check
        // for them first.
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9')) continue;
        if (c != ';') i--;
        break;
    }

    std::string_view entityName = s.substr(1, i - 1);
    if (!entityName.empty()) {
        if (rune x = lookup(entityName)) {
            appendRune(b, x);
            return i;
        }
        if (const rune* x2 = lookup2(entityName)) {
            appendRune(b, x2[0]);
            appendRune(b, x2[1]);
            return i;
        }
        std::size_t maxLen = std::min(entityName.size() - 1, longestEntityWithoutSemicolon);
        for (std::size_t j = maxLen; j > 1; j--) {
            if (rune x = lookup(entityName.substr(0, j))) {
                appendRune(b, x);
                return j + 1;
            }
        }
    }

    b += s.substr(0, i);
    return i;
}

}  // namespace

string EscapeString(const string& s) {
    if (s.view().find_first_of("&'<>\"") == std::string_view::npos) return s;
    std::string b;
    b.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': b += "&amp;"; break;
            case '\'': b += "&#39;"; break;  // "&#39;" is shorter than "&apos;" and apos was not in HTML until HTML5
            case '<': b += "&lt;"; break;
            case '>': b += "&gt;"; break;
            case '"': b += "&#34;"; break;  // "&#34;" is shorter than "&quot;"
            default: b += c;
        }
    }
    return string(b);
}

string UnescapeString(const string& s) {
    std::string_view v = s;
    std::size_t i = v.find('&');
    if (i == v.npos) return s;
    std::string b(v.substr(0, i));
    b.reserve(v.size());
    while (i < v.size()) {
        if (v[i] == '&') {
            i += unescapeEntity(b, v.substr(i));
            continue;
        }
        std::size_t j = v.find('&', i);
        if (j == v.npos) j = v.size();
        b += v.substr(i, j - i);
        i = j;
    }
    return string(b);
}

}  // namespace gopp::html
