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

namespace gopp::text::template_::detail {

using rune = int32;
inline constexpr rune rune_error = 0xFFFD;
inline constexpr rune max_rune = 0x10FFFF;

// utf8.DecodeRuneInString: the rune at s[i:] and its width; (RuneError, 1)
// for invalid encodings, (RuneError, 0) at the end.
std::pair<rune, int> decode_rune(std::string_view s, std::size_t i = 0) noexcept;
// utf8.DecodeLastRuneInString of s[:end].
std::pair<rune, int> decode_last_rune(std::string_view s, std::size_t end) noexcept;
void append_rune(std::string& out, rune r);
bool valid_utf8(std::string_view s) noexcept;
std::size_t rune_count(std::string_view s) noexcept;

// unicode.IsLetter, IsDigit, IsPrint, IsSpace: exact for ASCII and Latin-1;
// other code points are letters (and printable) unless they are known
// spaces or controls.
bool is_letter(rune r) noexcept;
bool is_digit(rune r) noexcept;
bool is_print(rune r) noexcept;
bool is_space(rune r) noexcept;

// strconv.Quote, QuoteRune, QuoteToASCII (ascii_only).
std::string quote(std::string_view s, bool ascii_only = false);
std::string quote_rune(rune r, bool ascii_only = false);

// strconv.UnquoteChar: (value, multibyte, tail, err).
std::tuple<rune, bool, std::string_view, gopp::error> unquote_char(std::string_view s, char quote);
// strconv.Unquote: "...", `...` and '...'.
std::pair<std::string, gopp::error> unquote(std::string_view s);

// strconv.ParseInt, ParseUint, ParseFloat with Go's syntax: base 0 takes
// 0x, 0o, 0b and 0 prefixes and underscores; floats take hex mantissas
// with p exponents, "inf" and "nan".
std::pair<int64, gopp::error> parse_int(std::string_view s, int base, int bit_size);
std::pair<uint64, gopp::error> parse_uint(std::string_view s, int base, int bit_size);
std::pair<double, gopp::error> parse_float(std::string_view s);

// strconv.FormatFloat(f, fmt, prec, 64) for fmt in 'e', 'E', 'f', 'g', 'G';
// prec -1 is the shortest representation that round-trips.
std::string format_float(double f, char fmt, int prec);

}  // namespace gopp::text::template_::detail
