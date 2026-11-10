#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>

namespace gopp::strings {

namespace {

constexpr char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }
constexpr char ascii_upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c; }
constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

}  // namespace

bool EqualFold(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

std::tuple<string, string, bool> Cut(const string& s, std::string_view sep) {
    auto i = s.find(sep);
    if (i == s.npos) return {s, string(), false};
    return {s.slice(0, i), s.slice(i + sep.size()), true};
}

string TrimSpace(const string& s) {
    std::size_t i = 0, j = s.size();
    while (i < j && is_space(s[i])) i++;
    while (j > i && is_space(s[j - 1])) j--;
    return s.slice(i, j);
}

string TrimLeft(const string& s, std::string_view cutset) {
    std::size_t i = 0;
    while (i < s.size() && cutset.find(s[i]) != cutset.npos) i++;
    return s.slice(i);
}

string TrimRight(const string& s, std::string_view cutset) {
    std::size_t j = s.size();
    while (j > 0 && cutset.find(s[j - 1]) != cutset.npos) j--;
    return s.slice(0, j);
}

string Trim(const string& s, std::string_view cutset) { return TrimRight(TrimLeft(s, cutset), cutset); }

string TrimPrefix(const string& s, std::string_view prefix) {
    return s.starts_with(prefix) ? s.slice(prefix.size()) : s;
}

string TrimSuffix(const string& s, std::string_view suffix) {
    return s.ends_with(suffix) ? s.slice(0, s.size() - suffix.size()) : s;
}

string ToLower(std::string_view s) {
    return string::build(s.size(), [&](char* p) {
        for (std::size_t i = 0; i < s.size(); i++) p[i] = ascii_lower(s[i]);
    });
}

string ToUpper(std::string_view s) {
    return string::build(s.size(), [&](char* p) {
        for (std::size_t i = 0; i < s.size(); i++) p[i] = ascii_upper(s[i]);
    });
}

string Repeat(std::string_view s, std::size_t count) {
    return string::build(s.size() * count, [&](char* p) {
        for (std::size_t i = 0; i < count; i++) std::memcpy(p + i * s.size(), s.data(), s.size());
    });
}

slice<string> Split(const string& s, std::string_view sep) {
    slice<string> out;
    if (sep.empty()) {
        for (std::size_t i = 0; i < s.size(); i++) out = append(out, s.slice(i, i + 1));
        return out;
    }
    std::size_t start = 0;
    for (;;) {
        auto i = s.find(sep, start);
        if (i == s.npos) break;
        out = append(out, s.slice(start, i));
        start = i + sep.size();
    }
    return append(out, s.slice(start));
}

string Join(std::span<const string> elems, std::string_view sep) {
    if (elems.empty()) return {};
    if (elems.size() == 1) return elems[0];
    std::size_t n = sep.size() * (elems.size() - 1);
    for (auto& e : elems) n += e.size();
    return string::build(n, [&](char* p) {
        for (std::size_t i = 0; i < elems.size(); i++) {
            if (i > 0) {
                std::memcpy(p, sep.data(), sep.size());
                p += sep.size();
            }
            std::memcpy(p, elems[i].data(), elems[i].size());
            p += elems[i].size();
        }
    });
}

}  // namespace gopp::strings

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

std::pair<int64, error> ParseInt(std::string_view s, int base, int bit_size) {
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
    if (bit_size < 64) {
        int64 lim = int64(1) << (bit_size - 1);
        if (v >= lim) return {lim - 1, make_ptr<NumError>("ParseInt", string(s), ErrRange)};
        if (v < -lim) return {-lim, make_ptr<NumError>("ParseInt", string(s), ErrRange)};
    }
    return {v, nullptr};
}

std::pair<uint64, error> ParseUint(std::string_view s, int base, int bit_size) {
    if (s.empty() || s[0] == '-' || s[0] == '+') return {0, make_ptr<NumError>("ParseUint", string(s), ErrSyntax)};
    uint64 v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v, base == 0 ? 10 : base);
    if (r.ec == std::errc::result_out_of_range) return {UINT64_MAX, make_ptr<NumError>("ParseUint", string(s), ErrRange)};
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return {0, make_ptr<NumError>("ParseUint", string(s), ErrSyntax)};
    if (bit_size < 64 && v >= (uint64(1) << bit_size)) return {(uint64(1) << bit_size) - 1, make_ptr<NumError>("ParseUint", string(s), ErrRange)};
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

}  // namespace gopp::strconv

namespace gopp::fmt::detail {

void write_stdout(std::string_view s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

}  // namespace gopp::fmt::detail
