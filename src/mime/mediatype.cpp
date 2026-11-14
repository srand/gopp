// After Go's mime/mediatype.go and mime/grammar.go.
#include "internal.hpp"

#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include <map>
#include <string>

namespace gopp::mime {

const error ErrInvalidMediaParameter = errors::New("mime: invalid media parameter");

namespace {

const error errNoMediaType = errors::New("mime: no media type");
const error errNoSlashAfterFirstToken = errors::New("mime: expected slash after first token");
const error errNoTokenAfterSlash = errors::New("mime: expected token after slash");
const error errUnexpectedContentAfterMediaSubtype = errors::New("mime: unexpected content after media subtype");
const error errDuplicateParamName = errors::New("mime: duplicate parameter name");

// RFC 1521's tspecials.
bool isTspecial(unsigned char c) noexcept {
    switch (c) {
    case '(': case ')': case '<': case '>': case '@': case ',': case ';': case ':':
    case '\\': case '"': case '/': case '[': case ']': case '?': case '=':
        return true;
    }
    return false;
}

// RFC 7230's tchar.
bool isTokenChar(unsigned char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '{': case '|': case '}': case '~':
        return true;
    }
    return false;
}

bool isToken(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!isTokenChar(c)) return false;
    }
    return true;
}

// encodedword.go's needsEncoding.
bool needs_encoding(std::string_view s) noexcept {
    for (unsigned char b : s) {
        if ((b < ' ' || b > '~') && b != '\t') return true;
    }
    return false;
}

// unicode.IsSpace for the ASCII and Latin-1 spaces, as UTF-8.
std::size_t spaceLen(std::string_view s) noexcept {
    if (s.empty()) return 0;
    switch (s[0]) {
    case '\t': case '\n': case '\v': case '\f': case '\r': case ' ':
        return 1;
    }
    // U+0085 (NEL) and U+00A0 (NBSP)
    if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xC2 &&
        (static_cast<unsigned char>(s[1]) == 0x85 || static_cast<unsigned char>(s[1]) == 0xA0)) {
        return 2;
    }
    return 0;
}

std::string_view trimLeftSpace(std::string_view s) noexcept {
    while (std::size_t n = spaceLen(s)) s.remove_prefix(n);
    return s;
}

bool ishex(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int unhex(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

std::pair<string, bool> percentHexUnescape(std::string_view s) {
    std::size_t percents = 0;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '%') {
            i++;
            continue;
        }
        percents++;
        if (i + 2 >= s.size() || !ishex(s[i + 1]) || !ishex(s[i + 2])) return {{}, false};
        i += 3;
    }
    if (percents == 0) return {string(s), true};
    std::string t;
    t.reserve(s.size() - 2 * percents);
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '%') {
            t.push_back(static_cast<char>(unhex(s[i + 1]) << 4 | unhex(s[i + 2])));
            i += 3;
        } else {
            t.push_back(s[i++]);
        }
    }
    return {string(t), true};
}

// Decodes an RFC 2231 extended value: charset'language'value.
std::pair<string, bool> decode2231Enc(std::string_view v) {
    auto q1 = v.find('\'');
    if (q1 == v.npos) return {{}, false};
    std::string_view charset = v.substr(0, q1);
    v = v.substr(q1 + 1);
    auto q2 = v.find('\'');
    if (q2 == v.npos) return {{}, false};
    std::string_view ext_other_vals = v.substr(q2 + 1);
    // Only us-ascii and utf-8 are supported, as in Go.
    string cs = strings::ToLower(charset);
    if (cs != "us-ascii" && cs != "utf-8") return {{}, false};
    return percentHexUnescape(ext_other_vals);
}

error checkMediaTypeDisposition(std::string_view s) {
    auto [typ, rest] = internal::consume_token(s);
    if (typ.empty()) return errNoMediaType;
    if (rest.empty()) return nullptr;
    if (!rest.starts_with('/')) return errNoSlashAfterFirstToken;
    rest.remove_prefix(1);
    auto [subtype, rest2] = internal::consume_token(rest);
    if (subtype.empty()) return errNoTokenAfterSlash;
    if (!rest2.empty()) return errUnexpectedContentAfterMediaSubtype;
    return nullptr;
}

}  // namespace

namespace internal {

// consumeToken: the token at the start of v and the rest; an empty token
// if there is none.
std::pair<std::string_view, std::string_view> consume_token(std::string_view v) {
    for (std::size_t i = 0; i < v.size(); i++) {
        if (!isTokenChar(static_cast<unsigned char>(v[i]))) return {v.substr(0, i), v.substr(i)};
    }
    return {v, {}};
}

// consumeValue: a token or a quoted string at the start of v, unquoted,
// and the rest; an empty value and v if there is none.
std::pair<string, std::string_view> consume_value(std::string_view v) {
    if (v.empty()) return {{}, {}};
    if (v[0] != '"') {
        auto [tok, rest] = consume_token(v);
        return {string(tok), rest};
    }
    // A quoted string.
    std::string buffer;
    for (std::size_t i = 1; i < v.size(); i++) {
        char r = v[i];
        if (r == '"') return {string(buffer), v.substr(i + 1)};
        // When MSIE sends a full file path (in "intranet mode"), it does
        // not escape backslashes: "C:\dev\go\foo.txt", not
        // "C:\\dev\\go\\foo.txt". Only unescape \ before a tspecial.
        if (r == '\\' && i + 1 < v.size() && isTspecial(static_cast<unsigned char>(v[i + 1]))) {
            buffer.push_back(v[i + 1]);
            i++;
            continue;
        }
        if (r == '\r' || r == '\n') return {{}, v};
        buffer.push_back(r);
    }
    // Did not find the end quote.
    return {{}, v};
}

// consumeMediaParam: "; name=value" at the start of v: the lowercase name,
// the value and the rest; an empty name and v if there is none.
std::tuple<string, string, std::string_view> consume_media_param(std::string_view v) {
    std::string_view rest = trimLeftSpace(v);
    if (!rest.starts_with(';')) return {{}, {}, v};
    rest = trimLeftSpace(rest.substr(1));
    auto [tok, after] = consume_token(rest);
    string param = strings::ToLower(tok);
    if (param.empty()) return {{}, {}, v};
    rest = trimLeftSpace(after);
    if (!rest.starts_with('=')) return {{}, {}, v};
    rest = trimLeftSpace(rest.substr(1));
    auto [value, rest2] = consume_value(rest);
    if (value.empty() && rest2.data() == rest.data() && rest2.size() == rest.size()) return {{}, {}, v};
    return {param, value, rest2};
}

}  // namespace internal

string FormatMediaType(std::string_view t, const Params& param) {
    static const char upperhex[] = "0123456789ABCDEF";
    std::string b;
    if (auto slash = t.find('/'); slash == t.npos) {
        if (!isToken(t)) return {};
        b += strings::ToLower(t).view();
    } else {
        std::string_view major = t.substr(0, slash), sub = t.substr(slash + 1);
        if (!isToken(major) || !isToken(sub)) return {};
        b += strings::ToLower(major).view();
        b += '/';
        b += strings::ToLower(sub).view();
    }
    // Params is ordered, as Go sorts the keys.
    for (const auto& [attribute, value] : param) {
        b += "; ";
        if (!isToken(attribute)) return {};
        b += strings::ToLower(attribute).view();

        bool need_enc = needs_encoding(value);
        if (need_enc) b += '*';  // RFC 2231 section 4
        b += '=';
        if (need_enc) {
            b += "utf-8''";
            for (unsigned char ch : value) {
                if (ch <= ' ' || ch >= 0x7F || ch == '*' || ch == '\'' || ch == '%' || isTspecial(ch)) {
                    b += '%';
                    b += upperhex[ch >> 4];
                    b += upperhex[ch & 0x0F];
                } else {
                    b += static_cast<char>(ch);
                }
            }
            continue;
        }
        if (isToken(value)) {
            b += value.view();
            continue;
        }
        b += '"';
        for (char c : value) {
            if (c == '"' || c == '\\') b += '\\';
            b += c;
        }
        b += '"';
    }
    return string(b);
}

std::tuple<string, Params, error> ParseMediaType(std::string_view v) {
    std::string_view base = v.substr(0, v.find(';'));
    string mediatype = strings::TrimSpace(strings::ToLower(base));
    if (auto err = checkMediaTypeDisposition(mediatype)) return {{}, {}, err};

    Params params;
    // Parameters split into continuations (RFC 2231): name => piece => value.
    std::map<string, std::map<string, string>> continuation;

    v = v.substr(base.size());
    while (!v.empty()) {
        v = trimLeftSpace(v);
        if (v.empty()) break;
        auto [key, value, rest] = internal::consume_media_param(v);
        if (key.empty()) {
            // Ignore a trailing semicolon.
            if (strings::TrimSpace(string(rest)) == ";") break;
            // Parse error.
            return {mediatype, {}, ErrInvalidMediaParameter};
        }

        Params* pmap = &params;
        if (auto star = key.find('*'); star != string::npos) {
            pmap = &continuation[key.slice(0, star)];
        }
        if (auto it = pmap->find(key); it != pmap->end() && it->second != value) {
            // Duplicate parameter names are incorrect, but we'll allow them
            // if they're equal.
            return {{}, {}, errDuplicateParamName};
        }
        (*pmap)[key] = value;
        v = rest;
    }

    // Stitch together any continuations or things with stars
    // (i.e. RFC 2231 things with stars: "foo*0" or "foo*").
    for (const auto& [key, piece_map] : continuation) {
        string single_part_key = key + "*";
        if (auto it = piece_map.find(single_part_key); it != piece_map.end()) {
            if (auto [decv, ok] = decode2231Enc(it->second); ok) params[key] = decv;
            continue;
        }

        std::string buf;
        bool valid = false;
        for (int n = 0;; n++) {
            string simple_part = fmt::Sprint("{}*{}", key, n);
            if (auto it = piece_map.find(simple_part); it != piece_map.end()) {
                valid = true;
                buf += it->second.view();
                continue;
            }
            auto it = piece_map.find(simple_part + "*");
            if (it == piece_map.end()) break;
            valid = true;
            if (n == 0) {
                if (auto [decv, ok] = decode2231Enc(it->second); ok) buf += decv.view();
            } else {
                auto [decv, ok] = percentHexUnescape(it->second);
                buf += decv.view();
            }
        }
        if (valid) params[key] = string(buf);
    }
    return {mediatype, std::move(params), nullptr};
}

}  // namespace gopp::mime
