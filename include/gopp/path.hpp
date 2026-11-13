// The path package: slash-separated paths, as in URLs and io/fs (for the
// operating system's paths, Linux's are slash-separated too).
//
//     path::Join("a", "b/../c")     // "a/c"
//     path::Match("*.go", "x.go")   // true
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/string.hpp>

#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#endif

namespace gopp::path {

// path.ErrBadPattern: a malformed pattern for Match.
extern const error ErrBadPattern;

// path.Clean: the shortest equivalent path: repeated slashes, "." and
// ".." elements resolved lexically; "." for an empty path.
string Clean(std::string_view path);
// path.Split: the directory, up to and including the last slash, and the
// file name.
std::pair<string, string> Split(std::string_view path);
// path.Join: the non-empty elements joined with slashes, then cleaned; ""
// if all are empty.
string Join(std::span<const std::string_view> elem);
string Join(std::initializer_list<std::string_view> elem);
template <typename... S>
    requires(sizeof...(S) > 0 && (std::is_convertible_v<const S&, std::string_view> && ...))
string Join(const S&... elem) {
    return Join({std::string_view(elem)...});
}
// path.Ext: the extension, from the last dot of the last element; "" if
// it has none.
string Ext(std::string_view path);
// path.Base: the last element, without trailing slashes; "." for "", "/"
// for slashes only.
string Base(std::string_view path);
// path.IsAbs: whether the path starts with a slash.
bool IsAbs(std::string_view path);
// path.Dir: all but the last element, cleaned.
string Dir(std::string_view path);
// path.Match: whether name matches the shell pattern: '*' (any sequence
// of non-slash characters), '?' (one non-slash character), '[...]'
// classes with ranges and '^' negation, and '\\' escapes. ErrBadPattern
// for a malformed pattern.
std::pair<bool, error> Match(std::string_view pattern, std::string_view name);

}  // namespace gopp::path
