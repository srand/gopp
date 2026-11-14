// Ported from Go 1.27.3's strings/strings.go, iter.go and builder.go, over
// the algorithms in text.hpp that it shares with bytes.
#include <gopp/strings.hpp>
#include <gopp/unicode/utf8.hpp>

#include "text.hpp"

#include <cstring>

namespace gopp::strings {

namespace {

// The part v of s, sharing s's buffer; "" for nil.
string sub(const string& s, std::string_view v) {
    if (v.data() == nullptr || v.empty()) return {};
    return s.Sub(v);
}

slice<string> subs(const string& s, const std::vector<std::string_view>& parts) {
    auto out = slice<string>::make(parts.size());
    for (std::size_t i = 0; i < parts.size(); i++) out[i] = sub(s, parts[i]);
    return out;
}

// s itself if r is empty (the result equals s), else r.
string orSame(const string& s, std::optional<std::string> r) { return r ? string(*r) : s; }

// s itself if the result r equals s, else r.
string sameOr(const string& s, std::string r) { return r == s.view() ? s : string(r); }

}  // namespace

bool ContainsAny(std::string_view s, std::string_view chars) noexcept { return text::indexAny(s, chars) >= 0; }
bool ContainsRune(std::string_view s, rune r) noexcept { return text::indexRune(s, r) >= 0; }
bool ContainsFunc(std::string_view s, std::function_ref<bool(rune)> f) { return text::indexFunc(s, f) >= 0; }

isize LastIndex(std::string_view s, std::string_view substr) noexcept { return text::lastIndex(s, substr); }
isize LastIndexByte(std::string_view s, char c) noexcept { return text::lastIndexByte(s, c); }
isize IndexRune(std::string_view s, rune r) noexcept { return text::indexRune(s, r); }
isize IndexAny(std::string_view s, std::string_view chars) noexcept { return text::indexAny(s, chars); }
isize LastIndexAny(std::string_view s, std::string_view chars) noexcept { return text::lastIndexAny(s, chars); }
isize IndexFunc(std::string_view s, std::function_ref<bool(rune)> f) { return text::indexFunc(s, f); }
isize LastIndexFunc(std::string_view s, std::function_ref<bool(rune)> f) { return text::lastIndexFunc(s, f); }
isize Count(std::string_view s, std::string_view substr) noexcept { return text::count(s, substr); }
int Compare(std::string_view a, std::string_view b) noexcept { return text::compare(a, b); }
bool EqualFold(std::string_view s, std::string_view t) noexcept { return text::equal_fold(s, t); }

std::tuple<string, string, bool> Cut(const string& s, std::string_view sep) {
    auto r = text::cut(s, sep);
    return {sub(s, r.before), sub(s, r.after), r.found};
}

std::tuple<string, string, bool> CutLast(const string& s, std::string_view sep) {
    auto r = text::cutLast(s, sep);
    return {sub(s, r.before), sub(s, r.after), r.found};
}

std::pair<string, bool> CutPrefix(const string& s, std::string_view prefix) {
    if (!s.starts_with(prefix)) return {s, false};
    return {s.slice(prefix.size()), true};
}

std::pair<string, bool> CutSuffix(const string& s, std::string_view suffix) {
    if (!s.ends_with(suffix)) return {s, false};
    return {s.slice(0, s.size() - suffix.size()), true};
}

string TrimSpace(const string& s) { return sub(s, text::trim_space(s)); }
string Trim(const string& s, std::string_view cutset) { return sub(s, text::trim(s, cutset)); }
string TrimLeft(const string& s, std::string_view cutset) { return sub(s, text::trimLeft(s, cutset)); }
string TrimRight(const string& s, std::string_view cutset) { return sub(s, text::trimRight(s, cutset)); }

string TrimPrefix(const string& s, std::string_view prefix) {
    return s.starts_with(prefix) ? s.slice(prefix.size()) : s;
}

string TrimSuffix(const string& s, std::string_view suffix) {
    return s.ends_with(suffix) ? s.slice(0, s.size() - suffix.size()) : s;
}

string TrimFunc(const string& s, std::function_ref<bool(rune)> f) { return sub(s, text::trimFunc(s, f)); }
string TrimLeftFunc(const string& s, std::function_ref<bool(rune)> f) { return sub(s, text::trimLeftFunc(s, f)); }
string TrimRightFunc(const string& s, std::function_ref<bool(rune)> f) { return sub(s, text::trimRightFunc(s, f)); }

slice<string> Split(const string& s, std::string_view sep) { return subs(s, text::genSplit(s, sep, 0, -1)); }

slice<string> SplitN(const string& s, std::string_view sep, isize n) {
    if (n == 0) return {};
    return subs(s, text::genSplit(s, sep, 0, n));
}

slice<string> SplitAfter(const string& s, std::string_view sep) {
    return subs(s, text::genSplit(s, sep, sep.size(), -1));
}

slice<string> SplitAfterN(const string& s, std::string_view sep, isize n) {
    if (n == 0) return {};
    return subs(s, text::genSplit(s, sep, sep.size(), n));
}

slice<string> Fields(const string& s) { return subs(s, text::fields(s)); }

slice<string> FieldsFunc(const string& s, std::function_ref<bool(rune)> f) { return subs(s, text::fieldsFunc(s, f)); }

std::generator<string> Lines(string s) {
    while (!s.empty()) {
        auto i = s.find('\n');
        std::size_t end = i == string::npos ? s.size() : i + 1;
        co_yield s.slice(0, end);
        s = s.slice(end);
    }
}

namespace {

std::generator<string> splitSeq(string s, string sep, std::size_t sep_save) {
    if (sep.empty()) {
        while (!s.empty()) {
            auto [_, size] = utf8::DecodeRuneInString(s);
            co_yield s.slice(0, std::size_t(size));
            s = s.slice(std::size_t(size));
        }
        co_return;
    }
    for (;;) {
        auto i = s.find(sep);
        if (i == string::npos) break;
        co_yield s.slice(0, i + sep_save);
        s = s.slice(i + sep.size());
    }
    co_yield s;
}

}  // namespace

std::generator<string> SplitSeq(string s, string sep) { return splitSeq(std::move(s), std::move(sep), 0); }

std::generator<string> SplitAfterSeq(string s, string sep) {
    std::size_t n = sep.size();
    return splitSeq(std::move(s), std::move(sep), n);
}

std::generator<string> FieldsSeq(string s) {
    isize start = -1;
    for (std::size_t i = 0; i < s.size();) {
        int size = 1;
        rune r = static_cast<byte>(s[i]);
        bool isSpace = text::asciiSpace(static_cast<byte>(s[i]));
        if (r >= utf8::RuneSelf) {
            std::tie(r, size) = utf8::DecodeRuneInString(s.view().substr(i));
            isSpace = unicode::IsSpace(r);
        }
        if (isSpace) {
            if (start >= 0) {
                co_yield s.slice(std::size_t(start), i);
                start = -1;
            }
        } else if (start < 0) {
            start = isize(i);
        }
        i += std::size_t(size);
    }
    if (start >= 0) co_yield s.slice(std::size_t(start));
}

std::generator<string> FieldsFuncSeq(string s, std::function<bool(rune)> f) {
    isize start = -1;
    for (std::size_t i = 0; i < s.size();) {
        auto [r, size] = utf8::DecodeRuneInString(s.view().substr(i));
        if (f(r)) {
            if (start >= 0) {
                co_yield s.slice(std::size_t(start), i);
                start = -1;
            }
        } else if (start < 0) {
            start = isize(i);
        }
        i += std::size_t(size);
    }
    if (start >= 0) co_yield s.slice(std::size_t(start));
}

string ToLower(const string& s) { return orSame(s, text::toLower(s)); }
string ToUpper(const string& s) { return orSame(s, text::toUpper(s)); }
string ToTitle(const string& s) { return sameOr(s, text::map(unicode::ToTitle, s)); }

string ToLowerSpecial(const unicode::SpecialCase& c, const string& s) {
    return sameOr(s, text::map([&](rune r) { return c.ToLower(r); }, s));
}

string ToUpperSpecial(const unicode::SpecialCase& c, const string& s) {
    return sameOr(s, text::map([&](rune r) { return c.ToUpper(r); }, s));
}

string ToTitleSpecial(const unicode::SpecialCase& c, const string& s) {
    return sameOr(s, text::map([&](rune r) { return c.ToTitle(r); }, s));
}

string ToValidUTF8(const string& s, std::string_view replacement) {
    return orSame(s, text::toValidUtf8(s, replacement));
}

string Map(std::function_ref<rune(rune)> mapping, const string& s) { return sameOr(s, text::map(mapping, s)); }

string Repeat(std::string_view s, isize count) { return string(text::repeat(s, count, "strings")); }

string Replace(const string& s, std::string_view old, std::string_view new_, isize n) {
    return orSame(s, text::replace(s, old, new_, n));
}

string ReplaceAll(const string& s, std::string_view old, std::string_view new_) { return Replace(s, old, new_, -1); }

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

string Clone(std::string_view s) { return string(s); }

void Builder::WriteRune(rune r) {
    byte buf[utf8::UTFMax];
    int n = utf8::EncodeRune(buf, r);
    buf_.append(reinterpret_cast<const char*>(buf), std::size_t(n));
}

}  // namespace gopp::strings
