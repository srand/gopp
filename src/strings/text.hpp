// The algorithms of Go's strings and bytes packages, which are the same in
// both, over std::string_view. strings and bytes wrap them: strings into
// gopp::string, bytes into slice<byte>.
//
// The functions that return parts of s return views into s. Where Go's
// bytes package returns nil, they return a view whose data() is nullptr
// (nil); bytes turns that into a nil slice, strings into "".
#pragma once

#include <gopp/base.hpp>
#include <gopp/unicode.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gopp::strings::text {

using sv = std::string_view;
using pred = std::function_ref<bool(rune)>;
using mapping = std::function_ref<rune(rune)>;

// Go's nil slice, as a view.
inline constexpr sv nil{};

// The ASCII white space of Fields and TrimSpace.
inline constexpr bool ascii_space(unsigned char c) noexcept {
    return c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == ' ';
}

// Searching.
isize index(sv s, sv sep) noexcept;
isize last_index(sv s, sv sep) noexcept;
isize index_byte(sv s, char c) noexcept;
isize last_index_byte(sv s, char c) noexcept;
isize index_rune(sv s, rune r) noexcept;
isize index_any(sv s, sv chars) noexcept;
isize last_index_any(sv s, sv chars) noexcept;
isize index_func(sv s, pred f, bool truth = true);
isize last_index_func(sv s, pred f, bool truth = true);
isize count(sv s, sv sep) noexcept;
bool equal_fold(sv s, sv t) noexcept;
int compare(sv a, sv b) noexcept;

// Trimming.
sv trim(sv s, sv cutset) noexcept;
sv trim_left(sv s, sv cutset) noexcept;
sv trim_right(sv s, sv cutset) noexcept;
sv trim_space(sv s) noexcept;
sv trim_func(sv s, pred f);
sv trim_left_func(sv s, pred f);
sv trim_right_func(sv s, pred f);

// Cut and CutLast: the text before and after sep, and whether it was
// found; s, nil and false if not.
struct cut_result {
    sv before, after;
    bool found;
};
cut_result cut(sv s, sv sep) noexcept;
cut_result cut_last(sv s, sv sep) noexcept;

// Splitting. Each part ends where the next part's separator starts, plus
// sep_save bytes of it (SplitAfter). n < 0 means all parts; the caller
// handles n == 0, where Go returns nil.
std::vector<sv> gen_split(sv s, sv sep, std::size_t sep_save, isize n);
std::vector<sv> fields(sv s);
std::vector<sv> fields_func(sv s, pred f);

// Building new text. The std::optional ones are empty when the result
// would equal s, so that strings can return s itself, as Go does.
std::string map(mapping f, sv s);
std::optional<std::string> to_upper(sv s);
std::optional<std::string> to_lower(sv s);
std::string repeat(sv s, isize count, const char* pkg);
std::optional<std::string> replace(sv s, sv old, sv new_, isize n);
std::optional<std::string> to_valid_utf8(sv s, sv replacement);

}  // namespace gopp::strings::text
