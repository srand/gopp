// Structured Field Values for HTTP (RFC 9651), after Go's
// net/http/internal/httpsfv: parsing that hands the members of a list or
// dictionary, as text, to a callback, and functions that parse the bare
// items. HTTP/2's Priority header (RFC 9218) is a dictionary.
#pragma once

#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <functional>
#include <string_view>
#include <utility>

namespace gopp::http::httpsfv {

// ParseList: calls f with each member (a bare item or an inner list) and
// its parameters, as text; whether s is a valid List.
bool ParseList(std::string_view s, const std::function<void(std::string_view member, std::string_view param)>& f);
// ParseBareInnerList: calls f with each item of an inner list ("(a b)").
bool ParseBareInnerList(std::string_view s, const std::function<void(std::string_view bareItem, std::string_view param)>& f);
// ParseItem: calls f with the bare item and its parameters.
bool ParseItem(std::string_view s, const std::function<void(std::string_view bareItem, std::string_view param)>& f);
// ParseDictionary: calls f with each key, its value (a bare item or an
// inner list; "?1" when it has none) and its parameters.
bool ParseDictionary(std::string_view s,
                     const std::function<void(std::string_view key, std::string_view val, std::string_view param)>& f);
// ParseParameter: calls f with each key and value (";a=1;b").
bool ParseParameter(std::string_view s, const std::function<void(std::string_view key, std::string_view val)>& f);

// The bare items: the value, and whether s is one exactly.
std::pair<int64, bool> ParseInteger(std::string_view s);
std::pair<double, bool> ParseDecimal(std::string_view s);
std::pair<string, bool> ParseString(std::string_view s);  // without the quotes, escapes kept
std::pair<string, bool> ParseToken(std::string_view s);
std::pair<slice<byte>, bool> ParseByteSequence(std::string_view s);  // the base64 text, not decoded
std::pair<bool, bool> ParseBoolean(std::string_view s);
std::pair<time::Time, bool> ParseDate(std::string_view s);
std::pair<string, bool> ParseDisplayString(std::string_view s);

}  // namespace gopp::http::httpsfv
