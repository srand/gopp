// Structured Field Values, ported from Go 1.27.3's
// net/http/internal/httpsfv/httpsfv.go.
#include "httpsfv.hpp"

#include <gopp/strings.hpp>
#include <gopp/unicode/utf8.hpp>

#include <string>

namespace gopp::http::httpsfv {

namespace {

// A parse step: what was consumed, what's left, and whether it parsed.
struct result {
    std::string_view consumed;
    std::string_view rest;
    bool ok;
};

result fail(std::string_view s) { return {{}, s, false}; }
result done(std::string_view s, std::size_t n) { return {s.substr(0, n), s.substr(n), true}; }

bool isLCAlpha(char b) { return b >= 'a' && b <= 'z'; }
bool isAlpha(char b) { return isLCAlpha(b) || (b >= 'A' && b <= 'Z'); }
bool isDigit(char b) { return b >= '0' && b <= '9'; }
bool isVChar(char b) { return b >= 0x21 && b <= 0x7e; }
bool isSP(char b) { return b == 0x20; }
bool isTChar(char b) {
    return isAlpha(b) || isDigit(b) || std::string_view("!#$%&'*+-.^_`|~").find(b) != std::string_view::npos;
}

std::size_t countLeftWhitespace(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

// Two lower-case hex digits as a byte.
bool decOctetHex(char ch1, char ch2, unsigned char& out) {
    auto dec = [](char in, int& v) {
        if (isDigit(in)) v = in - '0';
        else if (in >= 'a' && in <= 'f') v = in - 'a' + 10;
        else return false;
        return true;
    };
    int a, b;
    if (!dec(ch1, a) || !dec(ch2, b)) return false;
    out = static_cast<unsigned char>(a << 4 | b);
    return true;
}

result consumeBareItem(std::string_view s);

result consumeKey(std::string_view s) {
    if (s.empty() || (!isLCAlpha(s[0]) && s[0] != '*')) return fail(s);
    std::size_t i = 0;
    while (i < s.size() && (isLCAlpha(s[i]) || isDigit(s[i]) || std::string_view("_-.*").find(s[i]) != std::string_view::npos)) i++;
    return done(s, i);
}

result consumeParameter(std::string_view s, const std::function<void(std::string_view, std::string_view)>* f) {
    std::string_view rest = s;
    while (!rest.empty()) {
        std::string_view val = "?1";  // a parameter without a value is true
        if (rest[0] != ';') break;
        rest = rest.substr(1);
        rest = rest.substr(countLeftWhitespace(rest));
        auto key = consumeKey(rest);
        if (!key.ok) return fail(s);
        rest = key.rest;
        if (!rest.empty() && rest[0] == '=') {
            auto v = consumeBareItem(rest.substr(1));
            if (!v.ok) return fail(s);
            val = v.consumed;
            rest = v.rest;
        }
        if (f && *f) (*f)(key.consumed, val);
    }
    return done(s, s.size() - rest.size());
}

result consumeBareInnerList(std::string_view s, const std::function<void(std::string_view, std::string_view)>* f) {
    if (s.empty() || s[0] != '(') return fail(s);
    std::string_view rest = s.substr(1);
    while (!rest.empty()) {
        rest = rest.substr(countLeftWhitespace(rest));
        if (!rest.empty() && rest[0] == ')') {
            rest = rest.substr(1);
            break;
        }
        auto item = consumeBareItem(rest);
        if (!item.ok) return fail(s);
        auto param = consumeParameter(item.rest, nullptr);
        if (!param.ok) return fail(s);
        rest = param.rest;
        if (rest.empty() || (rest[0] != ')' && !isSP(rest[0]))) return fail(s);
        if (f && *f) (*f)(item.consumed, param.consumed);
    }
    return done(s, s.size() - rest.size());
}

result consumeItem(std::string_view s, const std::function<void(std::string_view, std::string_view)>* f) {
    auto item = consumeBareItem(s);
    if (!item.ok) return fail(s);
    auto param = consumeParameter(item.rest, nullptr);
    if (!param.ok) return fail(s);
    if (f && *f) (*f)(item.consumed, param.consumed);
    return done(s, s.size() - param.rest.size());
}

result consumeIntegerOrDecimal(std::string_view s) {
    std::size_t i = 0, signOffset = 0, periodIndex = 0;
    bool isDecimal = false;
    if (i < s.size() && s[i] == '-') {
        i++;
        signOffset++;
    }
    if (i >= s.size() || !isDigit(s[i])) return fail(s);
    while (i < s.size()) {
        char ch = s[i];
        if (isDigit(ch)) {
            i++;
            continue;
        }
        if (!isDecimal && ch == '.') {
            if (i - signOffset > 12) return fail(s);
            periodIndex = i;
            isDecimal = true;
            i++;
            continue;
        }
        break;
    }
    if (!isDecimal && i - signOffset > 15) return fail(s);
    if (isDecimal) {
        if (i - signOffset > 16) return fail(s);
        if (s[i - 1] == '.') return fail(s);
        if (i - periodIndex - 1 > 3) return fail(s);
    }
    return done(s, i);
}

result consumeString(std::string_view s) {
    if (s.empty() || s[0] != '"') return fail(s);
    for (std::size_t i = 1; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '\\') {
            if (i + 1 >= s.size()) return fail(s);
            i++;
            if (s[i] != '"' && s[i] != '\\') return fail(s);
        } else if (ch == '"') {
            return done(s, i + 1);
        } else if (!isVChar(ch) && !isSP(ch)) {
            return fail(s);
        }
    }
    return fail(s);
}

result consumeToken(std::string_view s) {
    if (s.empty() || (!isAlpha(s[0]) && s[0] != '*')) return fail(s);
    std::size_t i = 0;
    while (i < s.size() && (isTChar(s[i]) || s[i] == ':' || s[i] == '/')) i++;
    return done(s, i);
}

result consumeByteSequence(std::string_view s) {
    if (s.empty() || s[0] != ':') return fail(s);
    for (std::size_t i = 1; i < s.size(); i++) {
        char ch = s[i];
        if (ch == ':') return done(s, i + 1);
        if (!isAlpha(ch) && !isDigit(ch) && ch != '+' && ch != '/' && ch != '=') return fail(s);
    }
    return fail(s);
}

result consumeBoolean(std::string_view s) {
    if (s.size() >= 2 && (s.substr(0, 2) == "?0" || s.substr(0, 2) == "?1")) return done(s, 2);
    return fail(s);
}

result consumeDate(std::string_view s) {
    if (s.empty() || s[0] != '@') return fail(s);
    auto n = consumeIntegerOrDecimal(s.substr(1));
    if (!n.ok) return fail(s);
    if (n.consumed.find('.') != std::string_view::npos) return fail(s);
    return done(s, 1 + n.consumed.size());
}

result consumeDisplayString(std::string_view s) {
    if (s.size() <= 1 || s.substr(0, 2) != "%\"") return fail(s);
    std::string decoded;  // its percent-encoded bytes must be UTF-8
    for (std::size_t i = 2; i < s.size();) {
        char ch = s[i];
        if (!isVChar(ch) && !isSP(ch)) return fail(s);
        if (ch == '"') {
            if (!utf8::ValidString(decoded)) return fail(s);
            return done(s, i + 1);
        }
        if (ch == '%') {
            unsigned char b;
            if (i + 2 >= s.size() || !decOctetHex(s[i + 1], s[i + 2], b)) return fail(s);
            decoded.push_back(static_cast<char>(b));
            i += 3;
        } else {
            decoded.push_back(ch);
            i++;
        }
    }
    return fail(s);
}

result consumeBareItem(std::string_view s) {
    if (s.empty()) return fail(s);
    char ch = s[0];
    if (ch == '-' || isDigit(ch)) return consumeIntegerOrDecimal(s);
    if (ch == '"') return consumeString(s);
    if (ch == '*' || isAlpha(ch)) return consumeToken(s);
    if (ch == ':') return consumeByteSequence(s);
    if (ch == '?') return consumeBoolean(s);
    if (ch == '@') return consumeDate(s);
    if (ch == '%') return consumeDisplayString(s);
    return fail(s);
}

}  // namespace

bool ParseList(std::string_view s, const std::function<void(std::string_view, std::string_view)>& f) {
    while (!s.empty()) {
        result member = !s.empty() && s[0] == '(' ? consumeBareInnerList(s, nullptr) : consumeBareItem(s);
        if (!member.ok) return false;
        auto param = consumeParameter(member.rest, nullptr);
        if (!param.ok) return false;
        if (f) f(member.consumed, param.consumed);
        s = param.rest;
        s = s.substr(countLeftWhitespace(s));
        if (s.empty()) break;
        if (s[0] != ',') return false;
        s = s.substr(1);
        s = s.substr(countLeftWhitespace(s));
        if (s.empty()) return false;
    }
    return true;
}

bool ParseBareInnerList(std::string_view s, const std::function<void(std::string_view, std::string_view)>& f) {
    auto r = consumeBareInnerList(s, &f);
    return r.ok && r.rest.empty();
}

bool ParseItem(std::string_view s, const std::function<void(std::string_view, std::string_view)>& f) {
    auto r = consumeItem(s, &f);
    return r.ok && r.rest.empty();
}

bool ParseDictionary(std::string_view s,
                     const std::function<void(std::string_view, std::string_view, std::string_view)>& f) {
    while (!s.empty()) {
        std::string_view val = "?1";  // a key without a value is true
        auto key = consumeKey(s);
        if (!key.ok) return false;
        s = key.rest;
        if (!s.empty() && s[0] == '=') {
            s = s.substr(1);
            auto v = !s.empty() && s[0] == '(' ? consumeBareInnerList(s, nullptr) : consumeBareItem(s);
            if (!v.ok) return false;
            val = v.consumed;
            s = v.rest;
        }
        auto param = consumeParameter(s, nullptr);
        if (!param.ok) return false;
        if (f) f(key.consumed, val, param.consumed);
        s = param.rest;
        s = s.substr(countLeftWhitespace(s));
        if (s.empty()) break;
        if (s[0] == ',') s = s.substr(1);
        s = s.substr(countLeftWhitespace(s));
        if (s.empty()) return false;
    }
    return true;
}

bool ParseParameter(std::string_view s, const std::function<void(std::string_view, std::string_view)>& f) {
    auto r = consumeParameter(s, &f);
    return r.ok && r.rest.empty();
}

std::pair<int64, bool> ParseInteger(std::string_view s) {
    if (auto r = consumeIntegerOrDecimal(s); !r.ok || !r.rest.empty()) return {0, false};
    auto [n, err] = strconv::ParseInt(s, 10, 64);
    if (err) return {0, false};
    return {n, true};
}

std::pair<double, bool> ParseDecimal(std::string_view s) {
    if (auto r = consumeIntegerOrDecimal(s); !r.ok || !r.rest.empty()) return {0, false};
    if (s.find('.') == std::string_view::npos) return {0, false};
    auto [n, err] = strconv::ParseFloat(s, 64);
    if (err) return {0, false};
    return {n, true};
}

std::pair<string, bool> ParseString(std::string_view s) {
    if (auto r = consumeString(s); !r.ok || !r.rest.empty()) return {{}, false};
    return {string(s.substr(1, s.size() - 2)), true};
}

std::pair<string, bool> ParseToken(std::string_view s) {
    if (auto r = consumeToken(s); !r.ok || !r.rest.empty()) return {{}, false};
    return {string(s), true};
}

std::pair<slice<byte>, bool> ParseByteSequence(std::string_view s) {
    if (auto r = consumeByteSequence(s); !r.ok || !r.rest.empty()) return {{}, false};
    return {toBytes(s.substr(1, s.size() - 2)), true};
}

std::pair<bool, bool> ParseBoolean(std::string_view s) {
    if (auto r = consumeBoolean(s); !r.ok || !r.rest.empty()) return {false, false};
    return {s == "?1", true};
}

std::pair<time::Time, bool> ParseDate(std::string_view s) {
    if (auto r = consumeDate(s); !r.ok || !r.rest.empty()) return {{}, false};
    auto [n, ok] = ParseInteger(s.substr(1));
    if (!ok) return {{}, false};
    return {time::Unix(n, 0), true};
}

std::pair<string, bool> ParseDisplayString(std::string_view s) {
    if (auto r = consumeDisplayString(s); !r.ok || !r.rest.empty()) return {{}, false};
    s = s.substr(2, s.size() - 3);
    std::string b;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '%') {
            unsigned char decoded;
            decOctetHex(s[i + 1], s[i + 2], decoded);
            b.push_back(static_cast<char>(decoded));
            i += 3;
            continue;
        }
        b.push_back(s[i]);
        i++;
    }
    return {string(b), true};
}

}  // namespace gopp::http::httpsfv
