// The strings package, ported from Go 1.27.3, and the parts of strconv
// used so far.
//
//     strings::Fields("  a b  c ")               // {"a", "b", "c"}
//     strings::EqualFold("Straße", "STRASSE")    // false: simple folding only
//     strings::ReplaceAll("oink oink", "k", "ky")
//     for (auto line : strings::Lines(text)) { ... }
//
// The functions that only read take std::string_view. The ones that return
// parts of their input (Trim, Cut, Split, Fields, ...) take a gopp::string
// and share its buffer, as Go's substrings do, and so do the ones that may
// return their input unchanged (ToLower, Replace, Map, ...). Go's iterators
// (iter.Seq) are std::generators.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/unicode.hpp>

#include <functional>
#include <generator>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#endif

namespace gopp::strings {

inline bool HasPrefix(std::string_view s, std::string_view prefix) noexcept { return s.starts_with(prefix); }
inline bool HasSuffix(std::string_view s, std::string_view suffix) noexcept { return s.ends_with(suffix); }
inline bool Contains(std::string_view s, std::string_view substr) noexcept { return s.find(substr) != s.npos; }
// strings.ContainsAny: whether any of the runes of chars is in s.
bool ContainsAny(std::string_view s, std::string_view chars) noexcept;
// strings.ContainsRune
bool ContainsRune(std::string_view s, rune r) noexcept;
// strings.ContainsFunc: whether any rune of s satisfies f.
bool ContainsFunc(std::string_view s, std::function_ref<bool(rune)> f);

// strings.Index: the index of the first substr in s, or -1.
inline isize Index(std::string_view s, std::string_view substr) noexcept {
    auto i = s.find(substr);
    return i == s.npos ? -1 : static_cast<isize>(i);
}
inline isize IndexByte(std::string_view s, char c) noexcept {
    auto i = s.find(c);
    return i == s.npos ? -1 : static_cast<isize>(i);
}
// strings.LastIndex: the index of the last substr in s, or -1.
isize LastIndex(std::string_view s, std::string_view substr) noexcept;
isize LastIndexByte(std::string_view s, char c) noexcept;
// strings.IndexRune: the index of the first r in s, or -1. For RuneError,
// the first invalid UTF-8 sequence or U+FFFD.
isize IndexRune(std::string_view s, rune r) noexcept;
// strings.IndexAny: the index of the first of the runes of chars in s.
isize IndexAny(std::string_view s, std::string_view chars) noexcept;
// strings.LastIndexAny: the index of the last of the runes of chars in s.
isize LastIndexAny(std::string_view s, std::string_view chars) noexcept;
// strings.IndexFunc: the index of the first rune of s that satisfies f.
isize IndexFunc(std::string_view s, std::function_ref<bool(rune)> f);
// strings.LastIndexFunc: the index of the last rune of s that satisfies f.
isize LastIndexFunc(std::string_view s, std::function_ref<bool(rune)> f);
// strings.Count: the number of non-overlapping substr in s; for an empty
// substr, the number of runes plus one.
isize Count(std::string_view s, std::string_view substr) noexcept;
// strings.Compare: -1, 0 or +1 as a < b, a == b or a > b.
int Compare(std::string_view a, std::string_view b) noexcept;
// strings.EqualFold: whether s and t are equal under Unicode's simple case
// folding.
bool EqualFold(std::string_view s, std::string_view t) noexcept;

// strings.Cut: the text before and after the first sep, and whether found.
std::tuple<string, string, bool> Cut(const string& s, std::string_view sep);
// strings.CutLast: as Cut, around the last sep.
std::tuple<string, string, bool> CutLast(const string& s, std::string_view sep);
// strings.CutPrefix: s without prefix, and whether it had it.
std::pair<string, bool> CutPrefix(const string& s, std::string_view prefix);
// strings.CutSuffix: s without suffix, and whether it had it.
std::pair<string, bool> CutSuffix(const string& s, std::string_view suffix);

// strings.TrimSpace: s without leading and trailing white space
// (unicode::IsSpace).
string TrimSpace(const string& s);
// strings.Trim: s without the leading and trailing runes in cutset.
string Trim(const string& s, std::string_view cutset);
string TrimLeft(const string& s, std::string_view cutset);
string TrimRight(const string& s, std::string_view cutset);
string TrimPrefix(const string& s, std::string_view prefix);
string TrimSuffix(const string& s, std::string_view suffix);
// strings.TrimFunc: s without the leading and trailing runes that satisfy f.
string TrimFunc(const string& s, std::function_ref<bool(rune)> f);
string TrimLeftFunc(const string& s, std::function_ref<bool(rune)> f);
string TrimRightFunc(const string& s, std::function_ref<bool(rune)> f);

// strings.Split: the parts of s between the seps; for an empty sep, each
// UTF-8 sequence.
slice<string> Split(const string& s, std::string_view sep);
// strings.SplitN: as Split, but at most n parts, the last holding the rest
// (n < 0: all of them; n == 0: nil).
slice<string> SplitN(const string& s, std::string_view sep, isize n);
// strings.SplitAfter: as Split, keeping each sep at the end of its part.
slice<string> SplitAfter(const string& s, std::string_view sep);
slice<string> SplitAfterN(const string& s, std::string_view sep, isize n);
// strings.Fields: the runs of non-space runes (unicode::IsSpace) of s.
slice<string> Fields(const string& s);
// strings.FieldsFunc: the runs of runes of s that don't satisfy f.
slice<string> FieldsFunc(const string& s, std::function_ref<bool(rune)> f);

// strings.Lines: the newline-terminated lines of s, each with its "\n";
// the last may lack one.
std::generator<string> Lines(string s);
// strings.SplitSeq: Split's parts, one at a time.
std::generator<string> SplitSeq(string s, string sep);
// strings.SplitAfterSeq: SplitAfter's parts, one at a time.
std::generator<string> SplitAfterSeq(string s, string sep);
// strings.FieldsSeq: Fields' fields, one at a time.
std::generator<string> FieldsSeq(string s);
// strings.FieldsFuncSeq: FieldsFunc's fields, one at a time.
std::generator<string> FieldsFuncSeq(string s, std::function<bool(rune)> f);

// strings.ToLower: s with its runes mapped to lower case.
string ToLower(const string& s);
// strings.ToUpper: s with its runes mapped to upper case.
string ToUpper(const string& s);
// strings.ToTitle: s with its runes mapped to title case.
string ToTitle(const string& s);
// strings.ToLowerSpecial: as ToLower, giving priority to c's mappings.
string ToLowerSpecial(const unicode::SpecialCase& c, const string& s);
string ToUpperSpecial(const unicode::SpecialCase& c, const string& s);
string ToTitleSpecial(const unicode::SpecialCase& c, const string& s);
// strings.ToValidUTF8: s with each run of invalid UTF-8 replaced by
// replacement.
string ToValidUTF8(const string& s, std::string_view replacement);
// strings.Map: s with each rune mapped by mapping; a negative result drops
// the rune.
string Map(std::function_ref<rune(rune)> mapping, const string& s);
// strings.Repeat: count copies of s. Panics if count is negative or the
// result would overflow.
string Repeat(std::string_view s, isize count);
// strings.Replace: s with the first n old replaced by new_ (n < 0: all of
// them). An empty old matches at the start and after each UTF-8 sequence.
string Replace(const string& s, std::string_view old, std::string_view new_, isize n);
// strings.ReplaceAll
string ReplaceAll(const string& s, std::string_view old, std::string_view new_);
// strings.Join: the elements with sep between them.
string Join(std::span<const string> elems, std::string_view sep);
// strings.Clone: a copy of s in a buffer of its own.
string Clone(std::string_view s);

// strings.Builder
class Builder : public io::Writer {
public:
    std::pair<isize, error> Write(std::span<const byte> p) override {
        buf_.append(reinterpret_cast<const char*>(p.data()), p.size());
        return {static_cast<isize>(p.size()), nullptr};
    }
    void WriteString(std::string_view s) { buf_.append(s); }
    void WriteByte(char c) { buf_.push_back(c); }
    // Appends the UTF-8 encoding of r (RuneError's if r is invalid).
    void WriteRune(rune r);
    void Grow(std::size_t n) { buf_.reserve(buf_.size() + n); }
    std::size_t len() const noexcept { return buf_.size(); }
    std::size_t Len() const noexcept { return buf_.size(); }
    std::size_t Cap() const noexcept { return buf_.capacity(); }
    void reset() noexcept { buf_.clear(); }
    void Reset() noexcept { buf_.clear(); }
    std::string_view view() const noexcept { return buf_; }
    gopp::string str() const { return gopp::string(buf_); }
    gopp::string String() const { return gopp::string(buf_); }

private:
    std::string buf_;
};

// strings.NewReader
inline ptr<io::StringReader> NewReader(string s) { return make_ptr<io::StringReader>(std::move(s)); }

}  // namespace gopp::strings

namespace gopp::strconv {

// The error of a failed conversion (strconv.NumError).
struct NumError : errorInterface {
    string Func;
    string Num;
    gopp::error Err;  // error() hides the type name in this scope
    NumError(string f, string n, gopp::error e) : Func(std::move(f)), Num(std::move(n)), Err(std::move(e)) {}
    string Error() const override;
    const errorInterface* Unwrap() const noexcept override { return Err.get(); }
};

extern const error ErrRange;
extern const error ErrSyntax;

string Itoa(int64 i);
string FormatInt(int64 i, int base);
string FormatUint(uint64 i, int base);
std::pair<isize, error> Atoi(std::string_view s);
std::pair<int64, error> ParseInt(std::string_view s, int base = 10, int bitSize = 64);
std::pair<uint64, error> ParseUint(std::string_view s, int base = 10, int bitSize = 64);
std::pair<bool, error> ParseBool(std::string_view s);
// strconv.Quote, for ASCII and UTF-8 text.
string Quote(std::string_view s);
// strconv.FormatFloat for fmt 'e', 'E', 'f', 'g' or 'G': prec digits, or
// with prec -1 the fewest that read back as f. Only bitSize 64 is
// supported; others panic.
string FormatFloat(double f, char fmt, int prec, int bitSize);

}  // namespace gopp::strconv
