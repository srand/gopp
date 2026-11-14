// After Go's encoding/json: scanner.Go (validation), encode.Go (strings and
// floats), decode.Go (literals), indent.Go and stream.Go.
#include <gopp/encoding/json.hpp>
#include <gopp/strings.hpp>

#include <time.h>

#include <cmath>
#include <cstdio>

namespace gopp::json {

namespace {

constexpr std::size_t max_nesting_depth = 10000;

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Decodes the UTF-8 sequence at s[i]; returns the rune and its size, or
// U+FFFD and 1 for invalid bytes (utf8.DecodeRuneInString).
std::pair<rune, int> decode_rune(std::string_view s, std::size_t i) noexcept {
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) return {c0, 1};
    auto cont = [&](std::size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
    auto cb = [&](std::size_t k) { return static_cast<rune>(static_cast<unsigned char>(s[i + k]) & 0x3F); };
    if (c0 >= 0xC2 && c0 <= 0xDF && cont(1)) return {static_cast<rune>(c0 & 0x1F) << 6 | cb(1), 2};
    if (c0 >= 0xE0 && c0 <= 0xEF && cont(1) && cont(2)) {
        rune r = static_cast<rune>(c0 & 0x0F) << 12 | cb(1) << 6 | cb(2);
        if (r >= 0x800 && (r < 0xD800 || r > 0xDFFF)) return {r, 3};
    }
    if (c0 >= 0xF0 && c0 <= 0xF4 && cont(1) && cont(2) && cont(3)) {
        rune r = static_cast<rune>(c0 & 0x07) << 18 | cb(1) << 12 | cb(2) << 6 | cb(3);
        if (r >= 0x10000 && r <= 0x10FFFF) return {r, 4};
    }
    return {0xFFFD, 1};
}

void append_rune(std::string& out, rune r) {
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

// Go's quoteChar, for error messages.
std::string quote_char(char c) {
    if (c == '\'') return "'\\''";
    if (c == '"') return "'\"'";
    std::string q(strconv::Quote(std::string_view(&c, 1)).view());
    return "'" + q.substr(1, q.size() - 2) + "'";
}

// ---------------------------------------------------------------------------
// Validation (scanner.Go), by recursive descent.

enum class scan_result { ok, eof, error };

struct scanner {
    std::string_view s;
    std::size_t pos = 0;
    bool at_eof = true;  // whether the input ends at s.size()
    std::string msg{};
    std::size_t err_offset{};

    scan_result fail(std::string m) {
        msg = std::move(m);
        err_offset = pos + 1;
        return scan_result::error;
    }

    scan_result eof() {
        if (at_eof) {
            msg = "unexpected end of JSON input";
            err_offset = s.size();
            return scan_result::error;
        }
        return scan_result::eof;
    }

    void skip_ws() {
        while (pos < s.size() && is_space(s[pos])) pos++;
    }

    scan_result literal(std::string_view word) {
        for (std::size_t i = 0; i < word.size(); i++, pos++) {
            if (pos >= s.size()) return eof();
            if (s[pos] != word[i]) {
                return fail("invalid character " + quote_char(s[pos]) + " in literal " + std::string(word) +
                            " (expecting " + quote_char(word[i]) + ")");
            }
        }
        return scan_result::ok;
    }

    scan_result number() {
        auto bad = [&] { return fail("invalid character " + quote_char(s[pos]) + " in numeric literal"); };
        if (s[pos] == '-') {
            pos++;
            if (pos >= s.size()) return eof();
            if (!is_digit(s[pos])) return bad();
        }
        if (s[pos] == '0') {
            pos++;
        } else {
            while (pos < s.size() && is_digit(s[pos])) pos++;
        }
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            if (pos >= s.size()) return eof();
            if (!is_digit(s[pos])) return bad();
            while (pos < s.size() && is_digit(s[pos])) pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            if (pos >= s.size()) return eof();
            if (!is_digit(s[pos])) return bad();
            while (pos < s.size() && is_digit(s[pos])) pos++;
        }
        // A number at the end of a partial stream may continue.
        if (pos >= s.size() && !at_eof) return scan_result::eof;
        return scan_result::ok;
    }

    scan_result string_() {
        pos++;  // the opening quote
        for (;;) {
            if (pos >= s.size()) return eof();
            unsigned char c = static_cast<unsigned char>(s[pos]);
            if (c == '"') {
                pos++;
                return scan_result::ok;
            }
            if (c < 0x20) return fail("invalid character " + quote_char(s[pos]) + " in string literal");
            if (c == '\\') {
                pos++;
                if (pos >= s.size()) return eof();
                char e = s[pos];
                if (e == 'u') {
                    for (int i = 0; i < 4; i++) {
                        pos++;
                        if (pos >= s.size()) return eof();
                        char h = s[pos];
                        if (!is_digit(h) && !(h >= 'a' && h <= 'f') && !(h >= 'A' && h <= 'F')) {
                            return fail("invalid character " + quote_char(h) + " in \\u hexadecimal character escape");
                        }
                    }
                } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) {
                    return fail("invalid character " + quote_char(e) + " in string escape code");
                }
            }
            pos++;
        }
    }

    // Scans one value. Iterative, with an explicit stack of open objects and
    // arrays, so that deep nesting can't overflow a fiber's stack.
    scan_result value() {
        std::vector<char> stack;  // '{' or '['
        enum { want_value, want_key, after_value } state = want_value;
        for (;;) {
            switch (state) {
            case want_value: {
                skip_ws();
                if (pos >= s.size()) return eof();
                char c = s[pos];
                if (c == '{' || c == '[') {
                    if (stack.size() >= max_nesting_depth) return fail("exceeded max depth");
                    pos++;
                    skip_ws();
                    if (pos >= s.size()) return eof();
                    if (s[pos] == (c == '{' ? '}' : ']')) {
                        pos++;
                        state = after_value;
                        break;
                    }
                    stack.push_back(c);
                    state = c == '{' ? want_key : want_value;
                    break;
                }
                scan_result r;
                if (c == '"') r = string_();
                else if (c == 't') r = literal("true");
                else if (c == 'f') r = literal("false");
                else if (c == 'n') r = literal("null");
                else if (c == '-' || is_digit(c)) r = number();
                else return fail("invalid character " + quote_char(c) + " looking for beginning of value");
                if (r != scan_result::ok) return r;
                state = after_value;
                break;
            }
            case want_key:
                skip_ws();
                if (pos >= s.size()) return eof();
                if (s[pos] != '"') {
                    return fail("invalid character " + quote_char(s[pos]) + " looking for beginning of object key string");
                }
                if (auto r = string_(); r != scan_result::ok) return r;
                skip_ws();
                if (pos >= s.size()) return eof();
                if (s[pos] != ':') return fail("invalid character " + quote_char(s[pos]) + " after object key");
                pos++;
                state = want_value;
                break;
            case after_value:
                if (stack.empty()) return scan_result::ok;
                skip_ws();
                if (pos >= s.size()) return eof();
                if (stack.back() == '{') {
                    if (s[pos] == ',') {
                        pos++;
                        state = want_key;
                    } else if (s[pos] == '}') {
                        pos++;
                        stack.pop_back();
                    } else {
                        return fail("invalid character " + quote_char(s[pos]) + " after object key:value pair");
                    }
                } else {
                    if (s[pos] == ',') {
                        pos++;
                        state = want_value;
                    } else if (s[pos] == ']') {
                        pos++;
                        stack.pop_back();
                    } else {
                        return fail("invalid character " + quote_char(s[pos]) + " after array element");
                    }
                }
                break;
            }
        }
    }
};

gopp::error syntax_error(const scanner& sc) {
    auto e = make_ptr<SyntaxError>();
    e->msg = string(sc.msg);
    e->offset = static_cast<int64>(sc.err_offset);
    return e;
}

const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Errors

string UnmarshalTypeError::Error() const {
    if (!struct_name.empty() || !field.empty()) {
        return "json: cannot unmarshal " + value + " into C++ struct field " + struct_name + "." + field + " of type " + type;
    }
    return "json: cannot unmarshal " + value + " into C++ value of type " + type;
}

namespace detail {

bool EqualFold(std::string_view a, std::string_view b) noexcept { return strings::EqualFold(a, b); }

gopp::error validate(std::string_view data) {
    scanner sc{data};
    if (sc.value() != scan_result::ok) return syntax_error(sc);
    sc.skip_ws();
    if (sc.pos < data.size()) {
        sc.fail("invalid character " + quote_char(data[sc.pos]) + " after top-level value");
        return syntax_error(sc);
    }
    return nullptr;
}

std::pair<std::size_t, gopp::error> value_length(std::string_view s, bool at_eof) {
    scanner sc{s};
    sc.at_eof = at_eof;
    switch (sc.value()) {
    case scan_result::ok: return {sc.pos, nullptr};
    case scan_result::eof: return {0, nullptr};
    case scan_result::error: return {0, syntax_error(sc)};
    }
    return {0, nullptr};
}

// ---------------------------------------------------------------------------
// Encoding

void encoder::WriteString(std::string_view s) {
    static const char hex[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            bool safe = b >= 0x20 && b != '"' && b != '\\' && (!escape_html || (b != '<' && b != '>' && b != '&'));
            if (safe) {
                i++;
                continue;
            }
            out.append(s.substr(start, i - start));
            switch (b) {
            case '\\': out.append("\\\\"); break;
            case '"': out.append("\\\""); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                out.append("\\u00");
                out.push_back(hex[b >> 4]);
                out.push_back(hex[b & 0xF]);
            }
            i++;
            start = i;
            continue;
        }
        auto [r, size] = decode_rune(s, i);
        if (r == 0xFFFD && size == 1) {
            out.append(s.substr(start, i - start));
            out.append("\\ufffd");
            i += 1;
            start = i;
            continue;
        }
        // U+2028 and U+2029 end lines in JavaScript.
        if (r == 0x2028 || r == 0x2029) {
            out.append(s.substr(start, i - start));
            out.append("\\u202");
            out.push_back(hex[r & 0xF]);
            i += static_cast<std::size_t>(size);
            start = i;
            continue;
        }
        i += static_cast<std::size_t>(size);
    }
    out.append(s.substr(start));
    out.push_back('"');
}

void encoder::write_float(double f, int bits) {
    if (std::isnan(f) || std::isinf(f)) {
        if (!err) {
            auto e = make_ptr<UnsupportedValueError>();
            e->str = std::isnan(f) ? string::from_static("NaN") : f > 0 ? string::from_static("+Inf") : string::from_static("-Inf");
            err = e;
        }
        out.append("null");
        return;
    }
    // Like ES6: plain notation except for very small or large numbers.
    double a = std::fabs(f);
    bool exp = false;
    if (a != 0) {
        if (bits == 64) exp = a < 1e-6 || a >= 1e21;
        else exp = static_cast<float>(a) < 1e-6f || static_cast<float>(a) >= 1e21f;
    }
    // The shortest digits that round-trip, from to_chars in scientific
    // notation: "d.ddde+XX". (to_chars in fixed notation prints large
    // values with all the digits of their binary value, where Go's
    // strconv pads the shortest digits with zeros.)
    char buf[64];
    std::to_chars_result r;
    if (bits == 32) r = std::to_chars(buf, buf + sizeof(buf), static_cast<float>(f), std::chars_format::scientific);
    else r = std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::scientific);
    std::string_view sci(buf, static_cast<std::size_t>(r.ptr - buf));
    std::size_t e = sci.find('e');
    int exponent = 0;
    std::from_chars(sci.data() + e + (sci[e + 1] == '+' ? 2 : 1), sci.data() + sci.size(), exponent);
    if (exp) {
        // Go's 'e' format: mantissa, then e, sign and at least two digits,
        // cleaned up below to "e-7" from "e-07".
        out.append(sci.substr(0, e));
        char ebuf[8];
        int n = std::snprintf(ebuf, sizeof(ebuf), "e%c%02d", exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
        std::string_view es(ebuf, static_cast<std::size_t>(n));
        if (es.size() == 4 && es[1] == '-' && es[2] == '0') {
            out.append("e-");
            out.push_back(es[3]);
        } else {
            out.append(es);
        }
        return;
    }
    // Fixed notation from the digits and the exponent.
    std::string_view mant = sci.substr(0, e);
    if (!mant.empty() && mant[0] == '-') {
        out.push_back('-');
        mant.remove_prefix(1);
    }
    std::string digits;
    for (char c : mant) {
        if (c != '.') digits.push_back(c);
    }
    if (exponent >= 0) {
        std::size_t int_len = static_cast<std::size_t>(exponent) + 1;
        if (digits.size() <= int_len) {
            out.append(digits);
            out.append(int_len - digits.size(), '0');
        } else {
            out.append(digits, 0, int_len);
            out.push_back('.');
            out.append(digits, int_len);
        }
    } else {
        out.append("0.");
        out.append(static_cast<std::size_t>(-exponent - 1), '0');
        out.append(digits);
    }
}

void encoder::write_base64(std::span<const byte> b) {
    out.push_back('"');
    std::size_t i = 0;
    for (; i + 3 <= b.size(); i += 3) {
        uint32_t v = static_cast<uint32_t>(b[i]) << 16 | static_cast<uint32_t>(b[i + 1]) << 8 | b[i + 2];
        out.push_back(base64_chars[v >> 18]);
        out.push_back(base64_chars[(v >> 12) & 63]);
        out.push_back(base64_chars[(v >> 6) & 63]);
        out.push_back(base64_chars[v & 63]);
    }
    if (i < b.size()) {
        uint32_t v = static_cast<uint32_t>(b[i]) << 16;
        if (i + 1 < b.size()) v |= static_cast<uint32_t>(b[i + 1]) << 8;
        out.push_back(base64_chars[v >> 18]);
        out.push_back(base64_chars[(v >> 12) & 63]);
        out.push_back(i + 1 < b.size() ? base64_chars[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    out.push_back('"');
}

void encoder::write_value(const Value& v) {
    switch (v.type()) {
    case Value::kind::null: out.append("null"); break;
    case Value::kind::boolean: out.append(v.AsBool() ? "true" : "false"); break;
    case Value::kind::number: write_float(v.AsNumber(), 64); break;
    case Value::kind::string: WriteString(v.AsString()); break;
    case Value::kind::array: {
        out.push_back('[');
        bool first = true;
        for (const Value& x : v.AsArray()) {
            if (!first) out.push_back(',');
            first = false;
            write_value(x);
        }
        out.push_back(']');
        break;
    }
    case Value::kind::object: {
        out.push_back('{');
        bool first = true;
        for (const auto& [k, x] : v.AsObject()) {  // sorted by key
            if (!first) out.push_back(',');
            first = false;
            WriteString(k);
            out.push_back(':');
            write_value(x);
        }
        out.push_back('}');
        break;
    }
    }
}

void encoder::write_marshaled(std::pair<slice<byte>, gopp::error> r, std::string_view type) {
    auto fail = [&](gopp::error e) {
        if (!err) {
            auto me = make_ptr<MarshalerError>();
            me->type = string(type);
            me->Err = std::move(e);
            err = me;
        }
        out.append("null");
    };
    if (r.second) return fail(r.second);
    std::string_view s = as_string_view(r.first);
    if (gopp::error e = validate(s)) return fail(e);
    std::string compacted;
    append_compact(compacted, s, escape_html);
    out.append(compacted);
}

void encoder::cycle_error(std::string_view type) {
    if (err) return;
    auto e = make_ptr<UnsupportedValueError>();
    e->str = "encountered a cycle via " + string(type);
    err = e;
}

// Go's appendCompact: valid src without space outside strings; with
// escape, also escapes <, >, &, U+2028 and U+2029.
void append_compact(std::string& dst, std::string_view s, bool escape) {
    bool in_string = false;
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (escape) {
            if (c == '<' || c == '>' || c == '&') {
                dst.append(c == '<' ? "\\u003c" : c == '>' ? "\\u003e" : "\\u0026");
                continue;
            }
            // U+2028 is E2 80 A8, U+2029 is E2 80 A9.
            if (c == '\xe2' && i + 2 < s.size() && s[i + 1] == '\x80' && (s[i + 2] & ~1) == '\xa8') {
                dst.append(s[i + 2] == '\xa8' ? "\\u2028" : "\\u2029");
                i += 2;
                continue;
            }
        }
        if (in_string) {
            dst.push_back(c);
            if (c == '\\' && i + 1 < s.size()) dst.push_back(s[++i]);
            else if (c == '"') in_string = false;
        } else if (!is_space(c)) {
            dst.push_back(c);
            if (c == '"') in_string = true;
        }
    }
}

void indent(std::string& dst, std::string_view src, std::string_view prefix, std::string_view ind) {
    // After Go's appendIndent: src is valid JSON. Leading space is dropped,
    // trailing space kept.
    std::size_t end = src.size();
    while (end > 0 && is_space(src[end - 1])) end--;
    std::string_view trailing = src.substr(end);
    src = src.substr(0, end);
    int depth = 0;
    auto newline = [&] {
        dst.push_back('\n');
        dst.append(prefix);
        for (int i = 0; i < depth; i++) dst.append(ind);
    };
    bool in_string = false;
    for (std::size_t i = 0; i < src.size(); i++) {
        char c = src[i];
        if (in_string) {
            dst.push_back(c);
            if (c == '\\' && i + 1 < src.size()) dst.push_back(src[++i]);
            else if (c == '"') in_string = false;
            continue;
        }
        switch (c) {
        case '"':
            in_string = true;
            dst.push_back(c);
            break;
        case '{':
        case '[': {
            dst.push_back(c);
            std::size_t j = i + 1;
            while (j < src.size() && is_space(src[j])) j++;
            if (j < src.size() && (src[j] == '}' || src[j] == ']')) {
                dst.push_back(src[j]);  // empty: {} or []
                i = j;
            } else {
                depth++;
                newline();
            }
            break;
        }
        case '}':
        case ']':
            depth--;
            newline();
            dst.push_back(c);
            break;
        case ',':
            dst.push_back(c);
            newline();
            break;
        case ':':
            dst.append(": ");
            break;
        default:
            if (!is_space(c)) dst.push_back(c);
        }
    }
    dst.append(trailing);
}

// ---------------------------------------------------------------------------
// Decoding (of validated input)

void decoder::skip_ws() noexcept {
    while (pos_ < data_.size() && is_space(data_[pos_])) pos_++;
}

char decoder::peek() noexcept {
    skip_ws();
    return pos_ < data_.size() ? data_[pos_] : '\0';
}

bool decoder::consume_null() noexcept {
    if (peek() == 'n') {
        pos_ += 4;
        return true;
    }
    return false;
}

std::string_view decoder::literal() {
    skip_ws();
    std::size_t start = pos_;
    while (pos_ < data_.size()) {
        char c = data_[pos_];
        if (c == ',' || c == '}' || c == ']' || is_space(c)) break;
        pos_++;
    }
    return data_.substr(start, pos_ - start);
}

std::string_view decoder::raw() {
    skip_ws();
    std::size_t start = pos_;
    scanner sc{data_};
    sc.pos = pos_;
    sc.value();  // validated already
    pos_ = sc.pos;
    return data_.substr(start, pos_ - start);
}

void decoder::ReadString(std::string& out) {
    skip_ws();
    out.clear();
    pos_++;  // the opening quote
    std::size_t start = pos_;
    // Fast path: no escapes and valid UTF-8.
    for (std::size_t i = pos_; i < data_.size(); i++) {
        unsigned char c = static_cast<unsigned char>(data_[i]);
        if (c == '"') {
            out.assign(data_.substr(start, i - start));
            pos_ = i + 1;
            return;
        }
        if (c == '\\' || c >= 0x80) break;
    }
    while (pos_ < data_.size()) {
        unsigned char c = static_cast<unsigned char>(data_[pos_]);
        if (c == '"') {
            pos_++;
            return;
        }
        if (c == '\\') {
            char e = data_[pos_ + 1];
            pos_ += 2;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto hex4 = [&](std::size_t at) {
                    rune r = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = data_[at + i];
                        r = r << 4 | (is_digit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
                    }
                    return r;
                };
                rune r = hex4(pos_);
                pos_ += 4;
                if (r >= 0xD800 && r < 0xDC00) {
                    // A surrogate pair, if the low half follows.
                    if (pos_ + 6 <= data_.size() && data_[pos_] == '\\' && data_[pos_ + 1] == 'u') {
                        rune lo = hex4(pos_ + 2);
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            r = 0x10000 + ((r - 0xD800) << 10) + (lo - 0xDC00);
                            pos_ += 6;
                        } else {
                            r = 0xFFFD;
                        }
                    } else {
                        r = 0xFFFD;
                    }
                } else if (r >= 0xDC00 && r < 0xE000) {
                    r = 0xFFFD;
                }
                append_rune(out, r);
                break;
            }
            }
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            pos_++;
            continue;
        }
        auto [r, size] = decode_rune(data_, pos_);
        if (r == 0xFFFD && size == 1) append_rune(out, 0xFFFD);
        else out.append(data_.substr(pos_, static_cast<std::size_t>(size)));
        pos_ += static_cast<std::size_t>(size);
    }
}

void decoder::begin_object() noexcept {
    skip_ws();
    pos_++;
}

bool decoder::next_key(std::string& key) {
    skip_ws();
    if (data_[pos_] == '}') {
        pos_++;
        return false;
    }
    if (data_[pos_] == ',') pos_++;
    ReadString(key);
    skip_ws();
    pos_++;  // the colon
    return true;
}

void decoder::begin_array() noexcept {
    skip_ws();
    pos_++;
}

bool decoder::next_element() noexcept {
    skip_ws();
    if (data_[pos_] == ']') {
        pos_++;
        return false;
    }
    if (data_[pos_] == ',') pos_++;
    return true;
}

void decoder::read_value(Value& root) {
    // Iterative, like the scanner: a stack of the open arrays and objects.
    // Pointers into them stay valid, as a container only grows after its
    // nested containers are complete.
    struct frame {
        Value* v;
        bool object;
    };
    std::vector<frame> stack;
    std::string key;
    Value* target = &root;
    for (;;) {
        switch (peek()) {
        case 'n':
            pos_ += 4;
            *target = nullptr;
            break;
        case 't':
        case 'f':
            *target = literal() == "true";
            break;
        case '"': {
            std::string s;
            ReadString(s);
            *target = gopp::string(s);
            break;
        }
        case '[':
            begin_array();
            *target = Value::array();
            stack.push_back({target, false});
            break;
        case '{':
            begin_object();
            *target = Value::object();
            stack.push_back({target, true});
            break;
        default: {
            std::string_view lit = literal();
            double x = 0;
            std::from_chars(lit.data(), lit.data() + lit.size(), x);
            *target = x;
        }
        }
        // The next value to read, in the innermost open container.
        target = nullptr;
        while (!target) {
            if (stack.empty()) return;
            frame& f = stack.back();
            if (f.object) {
                if (next_key(key)) target = &f.v->AsObject()[std::string_view(key)];
                else stack.pop_back();
            } else {
                if (next_element()) target = &f.v->AsArray().emplace_back();
                else stack.pop_back();
            }
        }
    }
}

bool decoder::read_base64(slice<byte>& b) {
    std::string s;
    ReadString(s);
    std::string_view in = s;
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    if ((s.size() - in.size()) > 2 || (in.size() % 4 == 1)) return false;
    slice<byte> out = slice<byte>::make(0, in.size() * 3 / 4 + 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v = base64_value(c);
        if (v < 0) return false;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out = append(std::move(out), static_cast<byte>(acc >> bits));
        }
    }
    b = std::move(out);
    return true;
}

void decoder::mismatch(std::string_view type) {
    char c = peek();
    std::string what;
    switch (c) {
    case '"': what = "string"; break;
    case '{': what = "object"; break;
    case '[': what = "array"; break;
    case 't':
    case 'f': what = "bool"; break;
    case 'n': what = "null"; break;
    default: what = "number"; break;
    }
    bad_value(what, type);
    skip();
}

void decoder::bad_value(std::string_view value, std::string_view type) {
    fail(make_ptr<UnmarshalTypeError>(string(value), string(type), static_cast<int64>(pos_), string(struct_name), string(field)));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Value, RawMessage, valid

Value::~Value() {
    if (!IsArray() && !IsObject()) return;
    // Move the children out, and theirs, so that each is destroyed shallow.
    std::vector<Value> pending;
    auto take_children = [&](Value& v) {
        if (auto* a = std::get_if<array>(&v.v_)) {
            for (Value& c : *a) {
                if (c.IsArray() || c.IsObject()) pending.push_back(std::move(c));
            }
        } else if (auto* o = std::get_if<object>(&v.v_)) {
            for (auto& [k, c] : *o) {
                if (c.IsArray() || c.IsObject()) pending.push_back(std::move(c));
            }
        }
    };
    take_children(*this);
    while (!pending.empty()) {
        Value v = std::move(pending.back());
        pending.pop_back();
        take_children(v);
        v.v_ = nullptr;  // now shallow
    }
}

const Value& Value::operator[](std::string_view key) const {
    static const Value null_value;
    if (auto* o = std::get_if<object>(&v_)) {
        auto it = o->find(key);
        if (it != o->end()) return it->second;
    }
    return null_value;
}

Value& Value::operator[](std::string_view key) {
    if (IsNull()) v_ = object();
    object& o = get<object>();
    auto it = o.find(key);
    if (it == o.end()) it = o.emplace(gopp::string(key), Value()).first;
    return it->second;
}

const Value& Value::operator[](std::size_t i) const {
    const array& a = get<array>();
    if (i >= a.size()) panic("json::Value: index out of range");
    return a[i];
}

std::size_t Value::size() const noexcept {
    if (auto* a = std::get_if<array>(&v_)) return a->size();
    if (auto* o = std::get_if<object>(&v_)) return o->size();
    return 0;
}

std::pair<slice<byte>, gopp::error> RawMessage::MarshalJSON() const {
    if (data.is_nil()) return {to_bytes("null"), nullptr};
    return {data, nullptr};
}

gopp::error RawMessage::UnmarshalJSON(std::span<const byte> d) {
    data = slice<byte>::of(d);
    return nullptr;
}

bool Valid(std::span<const byte> data) { return !detail::validate(as_string_view(data)); }

error Compact(std::string& dst, std::string_view src) {
    if (error err = detail::validate(src)) return err;
    detail::append_compact(dst, src, false);
    return nullptr;
}

error Indent(std::string& dst, std::string_view src, std::string_view prefix, std::string_view ind) {
    if (error err = detail::validate(src)) return err;
    std::size_t start = 0;
    while (start < src.size() && is_space(src[start])) start++;
    detail::indent(dst, src.substr(start), prefix, ind);
    return nullptr;
}

void HTMLEscape(std::string& dst, std::string_view src) {
    for (std::size_t i = 0; i < src.size(); i++) {
        char c = src[i];
        if (c == '<' || c == '>' || c == '&') {
            dst.append(c == '<' ? "\\u003c" : c == '>' ? "\\u003e" : "\\u0026");
        } else if (c == '\xe2' && i + 2 < src.size() && src[i + 1] == '\x80' && (src[i + 2] & ~1) == '\xa8') {
            dst.append(src[i + 2] == '\xa8' ? "\\u2028" : "\\u2029");
            i += 2;
        } else {
            dst.push_back(c);
        }
    }
}

// ---------------------------------------------------------------------------
// Decoder

gopp::error Decoder::refill() {
    // Drop consumed bytes, then read more.
    if (scanp_ > 0) {
        offset_ += static_cast<int64>(scanp_);
        buf_.erase(0, scanp_);
        scanp_ = 0;
    }
    std::size_t old = buf_.size();
    buf_.resize(old + 4096);
    auto [n, err] = r_->Read(std::span<byte>(reinterpret_cast<byte*>(buf_.data() + old), 4096));
    buf_.resize(old + static_cast<std::size_t>(n));
    return err;
}

std::pair<std::string_view, error> Decoder::read_value() {
    for (;;) {
        std::string_view rest = std::string_view(buf_).substr(scanp_);
        bool at_eof = static_cast<bool>(err_);
        // Skip whitespace before checking for the end of the stream.
        std::size_t ws = 0;
        while (ws < rest.size() && is_space(rest[ws])) ws++;
        if (ws < rest.size() || at_eof) {
            if (ws == rest.size() && at_eof) {
                scanp_ = buf_.size();
                return {{}, err_};  // io::EOF_ at the end of the stream
            }
            auto [len, serr] = detail::value_length(rest, at_eof);
            if (serr) {
                err_ = serr;
                return {{}, serr};
            }
            if (len > 0) {
                scanp_ += len;
                return {rest.substr(0, len), nullptr};
            }
            if (at_eof) {
                err_ = io::ErrUnexpectedEOF;
                return {{}, err_};
            }
        }
        if (gopp::error e = refill()) err_ = e;
    }
}

bool Decoder::More() {
    for (;;) {
        std::string_view rest = std::string_view(buf_).substr(scanp_);
        std::size_t ws = 0;
        while (ws < rest.size() && is_space(rest[ws])) ws++;
        if (ws < rest.size()) return rest[ws] != ']' && rest[ws] != '}';
        if (err_) return false;
        if (gopp::error e = refill()) err_ = e;
    }
}

}  // namespace gopp::json
