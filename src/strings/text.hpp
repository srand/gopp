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
inline constexpr bool asciiSpace(unsigned char c) noexcept {
    return c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == ' ';
}

// Searching.
isize index(sv s, sv sep) noexcept;
isize lastIndex(sv s, sv sep) noexcept;
isize indexByte(sv s, char c) noexcept;
isize lastIndexByte(sv s, char c) noexcept;
isize indexRune(sv s, rune r) noexcept;
isize indexAny(sv s, sv chars) noexcept;
isize lastIndexAny(sv s, sv chars) noexcept;
isize indexFunc(sv s, pred f, bool truth = true);
isize lastIndexFunc(sv s, pred f, bool truth = true);
isize count(sv s, sv sep) noexcept;
bool equalFold(sv s, sv t) noexcept;
int compare(sv a, sv b) noexcept;

// Trimming.
sv trim(sv s, sv cutset) noexcept;
sv trimLeft(sv s, sv cutset) noexcept;
sv trimRight(sv s, sv cutset) noexcept;
sv trimSpace(sv s) noexcept;
sv trimFunc(sv s, pred f);
sv trimLeftFunc(sv s, pred f);
sv trimRightFunc(sv s, pred f);

// Cut and CutLast: the text before and after sep, and whether it was
// found; s, nil and false if not.
struct cutResult {
    sv before, after;
    bool found;
};
cutResult cut(sv s, sv sep) noexcept;
cutResult cutLast(sv s, sv sep) noexcept;

// Splitting. Each part ends where the next part's separator starts, plus
// sepSave bytes of it (SplitAfter). n < 0 means all parts; the caller
// handles n == 0, where Go returns nil.
std::vector<sv> genSplit(sv s, sv sep, std::size_t sepSave, isize n);
std::vector<sv> fields(sv s);
std::vector<sv> fieldsFunc(sv s, pred f);

// Building new text. The std::optional ones are empty when the result
// would equal s, so that strings can return s itself, as Go does.
std::string map(mapping f, sv s);
std::optional<std::string> toUpper(sv s);
std::optional<std::string> toLower(sv s);
std::string repeat(sv s, isize count, const char* pkg);
std::optional<std::string> replace(sv s, sv old, sv new_, isize n);
std::optional<std::string> toValidUtf8(sv s, sv replacement);

}  // namespace gopp::strings::text
