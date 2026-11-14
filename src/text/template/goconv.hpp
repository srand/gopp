// Go's strconv, unicode and utf8 pieces that text/template needs and gopp's
// strconv doesn't have: quoting and unquoting with Go's escapes, number
// parsing with base prefixes and underscores, and Go's float formatting.
// Internal to text/template.
#pragma once

#include <gopp/errors.hpp>
#include <gopp/string.hpp>

#include <string>
#include <string_view>
#include <tuple>

namespace gopp::text::tmpl::detail {

using rune = int32;
inline constexpr rune runeError = 0xFFFD;
inline constexpr rune maxRune = 0x10FFFF;

// utf8.DecodeRuneInString: the rune at s[i:] and its width; (RuneError, 1)
// for invalid encodings, (RuneError, 0) at the end.
std::pair<rune, int> decodeRune(std::string_view s, std::size_t i = 0) noexcept;
// utf8.DecodeLastRuneInString of s[:end].
std::pair<rune, int> decodeLastRune(std::string_view s, std::size_t end) noexcept;
void appendRune(std::string& out, rune r);
bool validUtf8(std::string_view s) noexcept;
std::size_t runeCount(std::string_view s) noexcept;

// unicode.IsLetter, IsDigit, IsPrint, IsSpace: exact for ASCII and Latin-1;
// other code points are letters (and printable) unless they are known
// spaces or controls.
bool isLetter(rune r) noexcept;
bool isDigit(rune r) noexcept;
bool isPrint(rune r) noexcept;
bool isSpace(rune r) noexcept;

// strconv.Quote, QuoteRune, QuoteToASCII (asciiOnly).
std::string quote(std::string_view s, bool asciiOnly = false);
std::string quoteRune(rune r, bool asciiOnly = false);

// strconv.UnquoteChar: (value, multibyte, tail, err).
std::tuple<rune, bool, std::string_view, gopp::error> unquoteChar(std::string_view s, char quote);
// strconv.Unquote: "...", `...` and '...'.
std::pair<std::string, gopp::error> unquote(std::string_view s);

// strconv.ParseInt, ParseUint, ParseFloat with Go's syntax: base 0 takes
// 0x, 0o, 0b and 0 prefixes and underscores; floats take hex mantissas
// with p exponents, "inf" and "nan".
std::pair<int64, gopp::error> parseInt(std::string_view s, int base, int bitSize);
std::pair<uint64, gopp::error> parseUint(std::string_view s, int base, int bitSize);
std::pair<double, gopp::error> parseFloat(std::string_view s);

// strconv.FormatFloat(f, fmt, prec, 64) for fmt in 'e', 'E', 'f', 'g', 'G';
// prec -1 is the shortest representation that round-trips.
std::string formatFloat(double f, char fmt, int prec);

}  // namespace gopp::text::tmpl::detail
