// After Go's strconv (quote.go, atoi.go, atof.go, ftoa.go) and unicode/utf8.
#include "goconv.hpp"

#include <gopp/strings.hpp>

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace gopp::text::template_::detail {

namespace {

const gopp::error err_syntax = errors::New("invalid syntax");
const gopp::error err_range = errors::New("value out of range");

gopp::error num_error(std::string_view fn, std::string_view s, const gopp::error& e) {
    return errors::New("strconv." + string(fn) + ": parsing " + string(quote(s)) + ": " + e.Error());
}

int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Removes underscores, checking Go's placement rule: only between digits
// (or after a base prefix).
bool strip_underscores(std::string_view s, std::string& out) {
    // underscoreOK
    char saw = '^';  // ^ start, 0 digit, _ underscore, ! other
    std::size_t i = 0;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) i = 1;
    bool hex = false;
    if (s.size() - i >= 2 && s[i] == '0') {
        char b = static_cast<char>(s[i + 1] | 0x20);
        if (b == 'b' || b == 'o' || b == 'x') {
            hex = b == 'x';
            i += 2;
            saw = '0';  // base prefix counts as a digit for "0x_1"
        }
    }
    for (; i < s.size(); i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (hex && hex_val(c) >= 0)) {
            saw = '0';
            continue;
        }
        if (c == '_') {
            if (saw != '0') return false;
            saw = '_';
            continue;
        }
        if (saw == '_') return false;
        saw = '!';
    }
    if (saw == '_') return false;
    out.clear();
    for (char c : s) {
        if (c != '_') out.push_back(c);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// UTF-8

std::pair<rune, int> decode_rune(std::string_view s, std::size_t i) noexcept {
    if (i >= s.size()) return {rune_error, 0};
    auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    unsigned char c = b(0);
    if (c < 0x80) return {c, 1};
    std::size_t n = s.size() - i;
    if (c >= 0xC2 && c <= 0xDF) {
        if (n < 2 || (b(1) & 0xC0) != 0x80) return {rune_error, 1};
        return {static_cast<rune>((c & 0x1F) << 6 | (b(1) & 0x3F)), 2};
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (b(1) & 0xC0) != 0x80 || (b(2) & 0xC0) != 0x80) return {rune_error, 1};
        rune r = (c & 0x0F) << 12 | (b(1) & 0x3F) << 6 | (b(2) & 0x3F);
        if (r < 0x800 || (r >= 0xD800 && r <= 0xDFFF)) return {rune_error, 1};
        return {r, 3};
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (b(1) & 0xC0) != 0x80 || (b(2) & 0xC0) != 0x80 || (b(3) & 0xC0) != 0x80) return {rune_error, 1};
        rune r = (c & 0x07) << 18 | (b(1) & 0x3F) << 12 | (b(2) & 0x3F) << 6 | (b(3) & 0x3F);
        if (r < 0x10000 || r > max_rune) return {rune_error, 1};
        return {r, 4};
    }
    return {rune_error, 1};
}

std::pair<rune, int> decode_last_rune(std::string_view s, std::size_t end) noexcept {
    if (end == 0) return {rune_error, 0};
    std::size_t start = end - 1;
    if (static_cast<unsigned char>(s[start]) < 0x80) return {static_cast<unsigned char>(s[start]), 1};
    std::size_t lim = end >= 4 ? end - 4 : 0;
    while (start > lim && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
    auto [r, w] = decode_rune(s.substr(0, end), start);
    if (start + static_cast<std::size_t>(w) != end) return {rune_error, 1};
    return {r, w};
}

void append_rune(std::string& out, rune r) {
    if (r < 0 || r > max_rune || (r >= 0xD800 && r <= 0xDFFF)) r = rune_error;
    if (r < 0x80) {
        out.push_back(static_cast<char>(r));
    } else if (r < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (r >> 6)));
        out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
    } else if (r < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (r >> 12)));
        out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (r >> 18)));
        out.push_back(static_cast<char>(0x80 | ((r >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((r >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (r & 0x3F)));
    }
}

bool valid_utf8(std::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size();) {
        auto [r, w] = decode_rune(s, i);
        if (r == rune_error && w == 1) return false;
        i += static_cast<std::size_t>(w);
    }
    return true;
}

std::size_t rune_count(std::string_view s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); n++) i += static_cast<std::size_t>(decode_rune(s, i).second);
    return n;
}

// ---------------------------------------------------------------------------
// unicode

bool is_letter(rune r) noexcept {
    if (r < 0x80) return (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z');
    if (r < 0x100) return r == 0xAA || r == 0xB5 || r == 0xBA || (r >= 0xC0 && r != 0xD7 && r != 0xF7);
    return !is_space(r) && r != rune_error && !(r >= 0x2000 && r <= 0x206F);
}

bool is_digit(rune r) noexcept {
    if (r < 0x80) return r >= '0' && r <= '9';
    // Other scripts' decimal digits (Unicode Nd), the common blocks.
    static const rune starts[] = {0x660, 0x6F0, 0x7C0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xBE6, 0xC66,
                                  0xCE6, 0xD66, 0xE50, 0xED0, 0xF20, 0x1040, 0x17E0, 0x1810, 0xFF10};
    for (rune s : starts) {
        if (r >= s && r < s + 10) return true;
    }
    return false;
}

bool is_space(rune r) noexcept {
    switch (r) {
    case '\t': case '\n': case '\v': case '\f': case '\r': case ' ': case 0x85: case 0xA0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    }
    return r >= 0x2000 && r <= 0x200A;
}

bool is_print(rune r) noexcept {
    if (r < 0x20 || (r >= 0x7F && r < 0xA0)) return false;
    if (r == 0xAD) return false;  // soft hyphen
    if (r == ' ') return true;
    if (r >= 0xD800 && r <= 0xDFFF) return false;
    if (r > max_rune) return false;
    if (r == 0xFEFF || r == 0xFFFE || r == 0xFFFF) return false;
    if ((r >= 0x2000 && r <= 0x200F) || (r >= 0x2028 && r <= 0x202E) || (r >= 0x2060 && r <= 0x206F)) return false;
    if (is_space(r)) return false;
    if (r >= 0xE000 && r <= 0xF8FF) return false;  // private use
    return true;
}

// ---------------------------------------------------------------------------
// Quoting

namespace {

void append_escaped_rune(std::string& out, rune r, char q, bool ascii_only) {
    if (r == q || r == '\\') {
        out.push_back('\\');
        out.push_back(static_cast<char>(r));
        return;
    }
    if (ascii_only) {
        if (r < 0x80 && is_print(r)) {
            out.push_back(static_cast<char>(r));
            return;
        }
    } else if (is_print(r)) {
        append_rune(out, r);
        return;
    }
    char buf[16];
    switch (r) {
    case '\a': out += "\\a"; return;
    case '\b': out += "\\b"; return;
    case '\f': out += "\\f"; return;
    case '\n': out += "\\n"; return;
    case '\r': out += "\\r"; return;
    case '\t': out += "\\t"; return;
    case '\v': out += "\\v"; return;
    }
    if (r < ' ' || r == 0x7f) {
        std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned>(r));
    } else if (r < 0x10000) {
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(r));
    } else {
        std::snprintf(buf, sizeof(buf), "\\U%08x", static_cast<unsigned>(r));
    }
    out += buf;
}

}  // namespace

std::string quote(std::string_view s, bool ascii_only) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (std::size_t i = 0; i < s.size();) {
        auto [r, w] = decode_rune(s, i);
        if (w == 1 && r == rune_error) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(s[i]));
            out += buf;
            i++;
            continue;
        }
        append_escaped_rune(out, r, '"', ascii_only);
        i += static_cast<std::size_t>(w);
    }
    out.push_back('"');
    return out;
}

std::string quote_rune(rune r, bool ascii_only) {
    if (r < 0 || r > max_rune || (r >= 0xD800 && r <= 0xDFFF)) r = rune_error;
    std::string out = "'";
    append_escaped_rune(out, r, '\'', ascii_only);
    out.push_back('\'');
    return out;
}

std::tuple<rune, bool, std::string_view, gopp::error> unquote_char(std::string_view s, char q) {
    if (s.empty()) return {0, false, s, err_syntax};
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (c == static_cast<unsigned char>(q) && (q == '\'' || q == '"')) return {0, false, s, err_syntax};
    if (c >= 0x80) {
        auto [r, w] = decode_rune(s);
        return {r, true, s.substr(static_cast<std::size_t>(w)), nullptr};
    }
    if (c != '\\') return {c, false, s.substr(1), nullptr};
    if (s.size() <= 1) return {0, false, s, err_syntax};
    char e = s[1];
    s = s.substr(2);
    rune value = 0;
    switch (e) {
    case 'a': value = '\a'; break;
    case 'b': value = '\b'; break;
    case 'f': value = '\f'; break;
    case 'n': value = '\n'; break;
    case 'r': value = '\r'; break;
    case 't': value = '\t'; break;
    case 'v': value = '\v'; break;
    case 'x': case 'u': case 'U': {
        int n = e == 'x' ? 2 : e == 'u' ? 4 : 8;
        if (static_cast<int>(s.size()) < n) return {0, false, s, err_syntax};
        uint32 v = 0;
        for (int j = 0; j < n; j++) {
            int x = hex_val(s[static_cast<std::size_t>(j)]);
            if (x < 0) return {0, false, s, err_syntax};
            v = v << 4 | static_cast<uint32>(x);
        }
        s = s.substr(static_cast<std::size_t>(n));
        if (e == 'x') return {static_cast<rune>(v), false, s, nullptr};
        if (v > static_cast<uint32>(max_rune) || (v >= 0xD800 && v <= 0xDFFF)) return {0, false, s, err_syntax};
        return {static_cast<rune>(v), true, s, nullptr};
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = e - '0';
        if (s.size() < 2) return {0, false, s, err_syntax};
        for (int j = 0; j < 2; j++) {
            int x = s[static_cast<std::size_t>(j)] - '0';
            if (x < 0 || x > 7) return {0, false, s, err_syntax};
            v = (v << 3) | x;
        }
        s = s.substr(2);
        if (v > 255) return {0, false, s, err_syntax};
        return {v, false, s, nullptr};
    }
    case '\\': value = '\\'; break;
    case '\'': case '"':
        if (e != q) return {0, false, s, err_syntax};
        value = e;
        break;
    default:
        return {0, false, s, err_syntax};
    }
    return {value, false, s, nullptr};
}

std::pair<std::string, gopp::error> unquote(std::string_view s) {
    if (s.size() < 2) return {{}, err_syntax};
    char q = s[0];
    if (q != s.back()) return {{}, err_syntax};
    std::string_view in = s.substr(1, s.size() - 2);
    if (q == '`') {
        if (in.find('`') != in.npos) return {{}, err_syntax};
        std::string out;
        for (char c : in) {
            if (c != '\r') out.push_back(c);  // raw strings drop carriage returns
        }
        return {out, nullptr};
    }
    if (q != '"' && q != '\'') return {{}, err_syntax};
    if (in.find('\n') != in.npos) return {{}, err_syntax};
    std::string out;
    std::size_t runes = 0;
    while (!in.empty()) {
        auto [r, multibyte, tail, err] = unquote_char(in, q);
        if (err) return {{}, err};
        in = tail;
        if (r < 0x80 && !multibyte) out.push_back(static_cast<char>(r));
        else append_rune(out, r);
        runes++;
        if (q == '\'' && !in.empty()) return {{}, err_syntax};
    }
    if (q == '\'' && runes != 1) return {{}, err_syntax};
    return {out, nullptr};
}

// ---------------------------------------------------------------------------
// Numbers

std::pair<uint64, gopp::error> parse_uint(std::string_view s0, int base, int bit_size) {
    if (s0.empty()) return {0, num_error("ParseUint", s0, err_syntax)};
    std::string_view s = s0;
    std::string clean;
    if (base == 0) {
        // Underscores are allowed only with base 0, between digits.
        if (!strip_underscores(s0, clean)) return {0, num_error("ParseUint", s0, err_syntax)};
        s = clean;
        base = 10;
        if (s[0] == '0') {
            char b = s.size() >= 2 ? static_cast<char>(s[1] | 0x20) : 0;
            if (s.size() >= 3 && b == 'b') {
                base = 2;
                s = s.substr(2);
            } else if (s.size() >= 3 && b == 'o') {
                base = 8;
                s = s.substr(2);
            } else if (s.size() >= 3 && b == 'x') {
                base = 16;
                s = s.substr(2);
            } else if (s.size() == 1) {
                return {0, nullptr};
            } else {
                base = 8;
                s = s.substr(1);
            }
        }
    }
    if (s.empty() || s[0] == '-' || s[0] == '+') return {0, num_error("ParseUint", s0, err_syntax)};
    uint64 v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (r.ptr != s.data() + s.size()) return {0, num_error("ParseUint", s0, err_syntax)};
    if (r.ec == std::errc::result_out_of_range) return {~uint64(0), num_error("ParseUint", s0, err_range)};
    if (r.ec != std::errc()) return {0, num_error("ParseUint", s0, err_syntax)};
    if (bit_size < 64 && v >= (uint64(1) << bit_size)) return {(uint64(1) << bit_size) - 1, num_error("ParseUint", s0, err_range)};
    return {v, nullptr};
}

std::pair<int64, gopp::error> parse_int(std::string_view s0, int base, int bit_size) {
    std::string_view s = s0;
    if (s.empty()) return {0, num_error("ParseInt", s0, err_syntax)};
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        s = s.substr(1);
    }
    auto [un, err] = parse_uint(s, base, bit_size);
    if (err) {
        if (err.Error().view().find("out of range") == std::string_view::npos) return {0, num_error("ParseInt", s0, err_syntax)};
        return {neg ? std::numeric_limits<int64>::min() : std::numeric_limits<int64>::max(), num_error("ParseInt", s0, err_range)};
    }
    uint64 cutoff = uint64(1) << (bit_size - 1);
    if (!neg && un >= cutoff) return {static_cast<int64>(cutoff - 1), num_error("ParseInt", s0, err_range)};
    if (neg && un > cutoff) return {-static_cast<int64>(cutoff), num_error("ParseInt", s0, err_range)};
    int64 n = neg ? static_cast<int64>(0 - un) : static_cast<int64>(un);
    return {n, nullptr};
}

std::pair<double, gopp::error> parse_float(std::string_view s0) {
    std::string_view s = s0;
    if (s.empty()) return {0, num_error("ParseFloat", s0, err_syntax)};
    std::string lower;
    for (char c : s) lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    std::string_view l = lower;
    bool neg = false;
    if (l[0] == '+' || l[0] == '-') {
        neg = l[0] == '-';
        l = l.substr(1);
    }
    if (l == "inf" || l == "infinity") return {neg ? -HUGE_VAL : HUGE_VAL, nullptr};
    if (l == "nan") return {std::nan(""), nullptr};
    std::string clean;
    if (s.find('_') != s.npos) {
        if (!strip_underscores(s, clean)) return {0, num_error("ParseFloat", s0, err_syntax)};
        s = clean;
    }
    std::string_view t = s;
    if (t[0] == '+' || t[0] == '-') t = t.substr(1);
    if (t.empty()) return {0, num_error("ParseFloat", s0, err_syntax)};
    double v = 0;
    std::from_chars_result r;
    if (t.size() > 2 && t[0] == '0' && (t[1] | 0x20) == 'x') {
        // Hex floats need a p exponent in Go.
        std::string_view h = t.substr(2);
        if (h.find_first_of("pP") == h.npos) return {0, num_error("ParseFloat", s0, err_syntax)};
        r = std::from_chars(h.data(), h.data() + h.size(), v, std::chars_format::hex);
        if (r.ptr != h.data() + h.size()) return {0, num_error("ParseFloat", s0, err_syntax)};
    } else {
        // Go accepts only decimal digits, '.', and an e exponent.
        for (char c : t) {
            if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) {
                return {0, num_error("ParseFloat", s0, err_syntax)};
            }
        }
        r = std::from_chars(t.data(), t.data() + t.size(), v, std::chars_format::general);
        if (r.ptr != t.data() + t.size() && r.ec == std::errc()) return {0, num_error("ParseFloat", s0, err_syntax)};
    }
    if (r.ec == std::errc::result_out_of_range) {
        // Overflow gives ±Inf, underflow 0, as Go.
        bool big = t.find_first_of("eEpP") != t.npos && t.find("-", t.find_first_of("eEpP")) == t.npos;
        double inf = big ? HUGE_VAL : 0;
        if (big) return {neg ? -inf : inf, num_error("ParseFloat", s0, err_range)};
        return {neg ? -0.0 : 0.0, nullptr};
    }
    if (r.ec != std::errc()) return {0, num_error("ParseFloat", s0, err_syntax)};
    return {neg ? -v : v, nullptr};
}

// ---------------------------------------------------------------------------
// FormatFloat

namespace {

// The decimal digits of f (without trailing zeros) and the decimal point
// position: f = 0.digits × 10^dp. prec < 0: shortest; otherwise rounded to
// prec significant digits.
void decimal_digits(double f, int prec, std::string& digs, int& dp) {
    char buf[512];
    std::to_chars_result r = prec < 0 ? std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::scientific)
                                      : std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::scientific, prec > 0 ? prec - 1 : 0);
    std::string_view s(buf, static_cast<std::size_t>(r.ptr - buf));
    auto e = s.find('e');
    std::string_view mant = s.substr(0, e);
    int exp = std::atoi(std::string(s.substr(e + 1)).c_str());
    digs.clear();
    for (char c : mant) {
        if (c >= '0' && c <= '9') digs.push_back(c);
    }
    while (digs.size() > 1 && digs.back() == '0') digs.pop_back();
    if (digs == "0") {
        digs.clear();
        dp = 0;
        return;
    }
    dp = exp + 1;
}

std::string fmt_e(bool neg, const std::string& d, int dp, int prec, char fmt) {
    std::string out;
    if (neg) out.push_back('-');
    out.push_back(d.empty() ? '0' : d[0]);
    if (prec > 0) {
        out.push_back('.');
        for (int i = 1; i <= prec; i++) out.push_back(i < static_cast<int>(d.size()) ? d[static_cast<std::size_t>(i)] : '0');
    }
    out.push_back(fmt);
    int exp = d.empty() ? 0 : dp - 1;
    out.push_back(exp < 0 ? '-' : '+');
    if (exp < 0) exp = -exp;
    if (exp < 10) out.push_back('0');
    out += std::to_string(exp);
    return out;
}

std::string fmt_f(bool neg, const std::string& d, int dp, int prec) {
    std::string out;
    if (neg) out.push_back('-');
    if (dp > 0) {
        for (int i = 0; i < dp; i++) out.push_back(i < static_cast<int>(d.size()) ? d[static_cast<std::size_t>(i)] : '0');
    } else {
        out.push_back('0');
    }
    if (prec > 0) {
        out.push_back('.');
        for (int i = 0; i < prec; i++) {
            int j = dp + i;
            out.push_back(j >= 0 && j < static_cast<int>(d.size()) ? d[static_cast<std::size_t>(j)] : '0');
        }
    }
    return out;
}

}  // namespace

std::string format_float(double f, char fmt, int prec) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "+Inf" : "-Inf";
    bool neg = std::signbit(f);
    double a = std::fabs(f);
    bool shortest = prec < 0;
    std::string d;
    int dp = 0;
    switch (fmt) {
    case 'e': case 'E': {
        decimal_digits(a, shortest ? -1 : prec + 1, d, dp);
        if (shortest) prec = d.empty() ? 0 : static_cast<int>(d.size()) - 1;
        return fmt_e(neg, d, dp, prec, fmt);
    }
    case 'f': {
        if (shortest) {
            decimal_digits(a, -1, d, dp);
            prec = std::max(static_cast<int>(d.size()) - dp, 0);
            return fmt_f(neg, d, dp, prec);
        }
        char buf[512];
        auto r = std::to_chars(buf, buf + sizeof(buf), a, std::chars_format::fixed, prec);
        return (neg ? "-" : "") + std::string(buf, r.ptr);
    }
    case 'g': case 'G': {
        if (prec == 0) prec = 1;
        decimal_digits(a, shortest ? -1 : prec, d, dp);
        int nd = static_cast<int>(d.size());
        if (shortest) prec = nd == 0 ? 1 : nd;
        int eprec = prec;
        if (eprec > nd && nd >= dp) eprec = nd;
        if (shortest) eprec = 6;
        int exp = dp - 1;
        if (nd == 0) exp = 0;
        if (exp < -4 || exp >= eprec) {
            if (prec > nd) prec = nd;
            return fmt_e(neg, d, dp, std::max(prec - 1, 0), fmt == 'g' ? 'e' : 'E');
        }
        if (prec > dp) prec = nd;
        return fmt_f(neg, d, dp, std::max(prec - dp, 0));
    }
    }
    return "%" + std::string(1, fmt);
}

}  // namespace gopp::text::template_::detail
