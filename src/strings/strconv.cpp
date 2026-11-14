#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include "text/template/goconv.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace gopp::strconv {

const error ErrRange = errors::New("value out of range");
const error ErrSyntax = errors::New("invalid syntax");

string NumError::Error() const { return "strconv." + Func + ": parsing " + Quote(Num) + ": " + Err.Error(); }

string Itoa(int64 i) { return FormatInt(i, 10); }

string FormatInt(int64 i, int base) {
    char buf[72];
    auto r = std::to_chars(buf, buf + sizeof(buf), i, base);
    return string(buf, static_cast<std::size_t>(r.ptr - buf));
}

string FormatUint(uint64 i, int base) {
    char buf[72];
    auto r = std::to_chars(buf, buf + sizeof(buf), i, base);
    return string(buf, static_cast<std::size_t>(r.ptr - buf));
}

namespace {

error numErr(const char* fn, std::string_view s, const error& e) { return make_ptr<NumError>(string::fromStatic(fn), string(s), e); }

constexpr char lower(char c) noexcept { return static_cast<char>(c | ('x' - 'X')); }

// underscoreOK reports whether the underscores in s are allowed: only
// with a base prefix, and only between digits or between the prefix and a
// digit.
bool underscoreOK(std::string_view s) {
    // saw tracks the last character (class) we saw: ^ for the beginning,
    // 0 for a digit or base prefix, _ for an underscore, ! for none of
    // the above.
    char saw = '^';
    std::size_t i = 0;
    // Optional sign.
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) s.remove_prefix(1);
    // Optional base prefix.
    bool hex = false;
    if (s.size() >= 2 && s[0] == '0' && (lower(s[1]) == 'b' || lower(s[1]) == 'o' || lower(s[1]) == 'x')) {
        i = 2;
        saw = '0';  // base prefix counts as a digit for "underscore as digit separator"
        hex = lower(s[1]) == 'x';
    }
    // Number proper.
    for (; i < s.size(); i++) {
        // Digits are always okay.
        if ((s[i] >= '0' && s[i] <= '9') || (hex && lower(s[i]) >= 'a' && lower(s[i]) <= 'f')) {
            saw = '0';
            continue;
        }
        // Underscore must follow digit.
        if (s[i] == '_') {
            if (saw != '0') return false;
            saw = '_';
            continue;
        }
        // Underscore must also be followed by digit.
        if (saw == '_') return false;
        // Saw non-digit, non-underscore.
        saw = '!';
    }
    return saw != '_';
}

}  // namespace

// After Go's strconv/atoi.go.
std::pair<uint64, error> ParseUint(std::string_view s, int base, int bitSize) {
    if (s.empty()) return {0, numErr("ParseUint", s, ErrSyntax)};
    bool base0 = base == 0;
    std::string_view s0 = s;
    if (base >= 2 && base <= 36) {
        // valid base; nothing to do
    } else if (base == 0) {
        // Look for octal, hex prefix.
        base = 10;
        if (s[0] == '0') {
            if (s.size() >= 3 && lower(s[1]) == 'b') {
                base = 2;
                s.remove_prefix(2);
            } else if (s.size() >= 3 && lower(s[1]) == 'o') {
                base = 8;
                s.remove_prefix(2);
            } else if (s.size() >= 3 && lower(s[1]) == 'x') {
                base = 16;
                s.remove_prefix(2);
            } else {
                base = 8;
                s.remove_prefix(1);
            }
        }
    } else {
        return {0, numErr("ParseUint", s0, errors::New("invalid base " + Itoa(base)))};
    }
    if (bitSize == 0) bitSize = 64;
    else if (bitSize < 0 || bitSize > 64) return {0, numErr("ParseUint", s0, errors::New("invalid bit size " + Itoa(bitSize)))};

    // Cutoff is the smallest number such that cutoff*base > maxUint64.
    uint64 cutoff = UINT64_MAX / static_cast<uint64>(base) + 1;
    uint64 maxVal = bitSize == 64 ? UINT64_MAX : (uint64(1) << bitSize) - 1;
    bool underscores = false;
    uint64 n = 0;
    for (char c : s) {
        uint64 d;
        if (c == '_' && base0) {
            underscores = true;
            continue;
        } else if (c >= '0' && c <= '9') {
            d = static_cast<uint64>(c - '0');
        } else if (lower(c) >= 'a' && lower(c) <= 'z') {
            d = static_cast<uint64>(lower(c) - 'a' + 10);
        } else {
            return {0, numErr("ParseUint", s0, ErrSyntax)};
        }
        if (d >= static_cast<uint64>(base)) return {0, numErr("ParseUint", s0, ErrSyntax)};
        if (n >= cutoff) return {maxVal, numErr("ParseUint", s0, ErrRange)};  // n*base overflows
        n *= static_cast<uint64>(base);
        uint64 n1 = n + d;
        if (n1 < n || n1 > maxVal) return {maxVal, numErr("ParseUint", s0, ErrRange)};  // n+d overflows
        n = n1;
    }
    if (underscores && !underscoreOK(s0)) return {0, numErr("ParseUint", s0, ErrSyntax)};
    return {n, nullptr};
}

std::pair<int64, error> ParseInt(std::string_view s, int base, int bitSize) {
    if (s.empty()) return {0, numErr("ParseInt", s, ErrSyntax)};
    // Pick off leading sign.
    std::string_view s0 = s;
    bool neg = false;
    if (s[0] == '+') {
        s.remove_prefix(1);
    } else if (s[0] == '-') {
        neg = true;
        s.remove_prefix(1);
    }
    // Convert unsigned and check range.
    auto [un, err] = ParseUint(s, base, bitSize);
    if (err) {
        auto* ne = errors::As<NumError>(err);
        if (!ne || ne->Err != ErrRange) return {0, numErr("ParseInt", s0, ne ? ne->Err : err)};
    }
    if (bitSize == 0) bitSize = 64;
    uint64 cutoff = uint64(1) << (bitSize - 1);
    if (!neg && un >= cutoff) return {static_cast<int64>(cutoff - 1), numErr("ParseInt", s0, ErrRange)};
    if (neg && un > cutoff) return {-static_cast<int64>(cutoff), numErr("ParseInt", s0, ErrRange)};
    int64 n = static_cast<int64>(un);
    if (neg) n = static_cast<int64>(0 - un);
    return {n, nullptr};
}

std::pair<isize, error> Atoi(std::string_view s) {
    auto [v, err] = ParseInt(s, 10, 64);
    if (err) {
        if (auto* ne = errors::As<NumError>(err)) return {static_cast<isize>(v), make_ptr<NumError>("Atoi", ne->Num, ne->Err)};
    }
    return {static_cast<isize>(v), err};
}

std::pair<bool, error> ParseBool(std::string_view s) {
    if (s == "1" || s == "t" || s == "T" || s == "true" || s == "TRUE" || s == "True") return {true, nullptr};
    if (s == "0" || s == "f" || s == "F" || s == "false" || s == "FALSE" || s == "False") return {false, nullptr};
    return {false, make_ptr<NumError>("ParseBool", string(s), ErrSyntax)};
}

string Quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[5];
                std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return string(out);
}

string FormatFloat(double f, char fmt, int prec, int bitSize) {
    if (bitSize != 64 && bitSize != 32) panic("strconv: illegal FormatFloat bitSize");
    if (bitSize == 32 && prec < 0 && std::isfinite(f)) {
        // The shortest digits that read back as the float32: as a decimal,
        // which the float64 formatting keeps (it has digits to spare).
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof(buf), static_cast<float>(f), std::chars_format::scientific);
        double d = 0;
        std::from_chars(buf, r.ptr, d);
        return string(text::tmpl::detail::formatFloat(d, fmt, prec));
    }
    return string(text::tmpl::detail::formatFloat(f, fmt, prec));
}

std::pair<double, error> ParseFloat(std::string_view s, int bitSize) {
    auto [v, err] = text::tmpl::detail::parseFloat(s);
    if (err) {
        bool range = err.Error().view().find("out of range") != std::string_view::npos;
        if (!range) return {0, numErr("ParseFloat", s, ErrSyntax)};
        return {bitSize == 32 ? static_cast<float>(v) : v, numErr("ParseFloat", s, ErrRange)};
    }
    if (bitSize != 32) return {v, nullptr};
    // Out of float32's range: ±Inf and ErrRange, as Go.
    float f = static_cast<float>(v);
    if (std::isinf(f) && !std::isinf(v)) return {f, numErr("ParseFloat", s, ErrRange)};
    return {f, nullptr};
}

}  // namespace gopp::strconv

namespace gopp::fmt::detail {

void writeStdout(std::string_view s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

namespace {

// Go's fmt.wrapError and wrapErrors.
struct wrapError final : errorInterface {
    wrapError(string m, std::vector<gopp::error> e) : msg(std::move(m)), errs(std::move(e)) {}
    string Error() const override { return msg; }
    const errorInterface* Unwrap() const noexcept override { return errs.size() == 1 ? errs[0].get() : nullptr; }
    std::span<const gopp::error> UnwrapList() const noexcept override {
        if (errs.size() == 1) return {};
        return errs;
    }
    string msg;
    std::vector<gopp::error> errs;
};

}  // namespace

error wrapErrors(string msg, std::vector<gopp::error> wrapped) {
    // Go: a nil %w operand isn't wrapped.
    std::erase_if(wrapped, [](const gopp::error& e) { return !e; });
    return make_ptr<wrapError>(std::move(msg), std::move(wrapped));
}

}  // namespace gopp::fmt::detail
