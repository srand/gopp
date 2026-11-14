#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include "text/template/goconv.hpp"

#include <cerrno>
#include <charconv>
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

std::pair<int64, error> ParseInt(std::string_view s, int base, int bitSize) {
    if (s.empty()) return {0, make_ptr<NumError>("ParseInt", string(s), ErrSyntax)};
    std::string_view t = s;
    if (t[0] == '+') t.remove_prefix(1);  // from_chars rejects '+'
    if (base == 0) {
        base = 10;
        std::string_view u = t[0] == '-' ? t.substr(1) : t;
        if (u.size() > 1 && u[0] == '0') {
            char b = static_cast<char>(u[1] | 0x20);
            base = b == 'x' ? 16 : b == 'o' ? 8 : b == 'b' ? 2 : 8;
            (void)b;
        }
        if (base != 10) return {0, make_ptr<NumError>("ParseInt", string(s), errors::New("base prefixes not supported"))};
    }
    int64 v = 0;
    auto r = std::from_chars(t.data(), t.data() + t.size(), v, base);
    if (r.ec == std::errc::result_out_of_range) return {v < 0 ? INT64_MIN : INT64_MAX, make_ptr<NumError>("ParseInt", string(s), ErrRange)};
    if (r.ec != std::errc() || r.ptr != t.data() + t.size()) return {0, make_ptr<NumError>("ParseInt", string(s), ErrSyntax)};
    if (bitSize < 64) {
        int64 lim = int64(1) << (bitSize - 1);
        if (v >= lim) return {lim - 1, make_ptr<NumError>("ParseInt", string(s), ErrRange)};
        if (v < -lim) return {-lim, make_ptr<NumError>("ParseInt", string(s), ErrRange)};
    }
    return {v, nullptr};
}

std::pair<uint64, error> ParseUint(std::string_view s, int base, int bitSize) {
    if (s.empty() || s[0] == '-' || s[0] == '+') return {0, make_ptr<NumError>("ParseUint", string(s), ErrSyntax)};
    uint64 v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v, base == 0 ? 10 : base);
    if (r.ec == std::errc::result_out_of_range) return {UINT64_MAX, make_ptr<NumError>("ParseUint", string(s), ErrRange)};
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return {0, make_ptr<NumError>("ParseUint", string(s), ErrSyntax)};
    if (bitSize < 64 && v >= (uint64(1) << bitSize)) return {(uint64(1) << bitSize) - 1, make_ptr<NumError>("ParseUint", string(s), ErrRange)};
    return {v, nullptr};
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
    if (bitSize != 64) panic("strconv: illegal FormatFloat bitSize");
    return string(text::tmpl::detail::formatFloat(f, fmt, prec));
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
