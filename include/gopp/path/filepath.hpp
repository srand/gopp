// The path/filepath package: the operating system's paths, ported from Go
// 1.27.3 for Linux, where they are slash-separated.
//
//     filepath::Join(dir, "conf", name + ".json");
//     auto [abs, err] = filepath::Abs("data");
//     auto [rel, err] = filepath::Rel("/a/b", "/a/c/d");   // "../c/d"
//     filepath::WalkDir(".", [](std::string_view path, const ptr<fs::DirEntry>& d, error err) -> error {
//         if (err) return err;
//         if (d->IsDir() && d->Name() == ".git") return filepath::SkipDir;
//         fmt::Println(path);
//         return nullptr;
//     });
//
// The lexical functions (Clean, Join, Split, Base, Dir, Ext, IsAbs, Match)
// are path's, as Linux paths are slash-separated.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/path.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <functional>
#include <initializer_list>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#endif

namespace gopp::filepath {

// filepath.Separator and filepath.ListSeparator: '/' and ':' on Linux.
inline constexpr char Separator = '/';
inline constexpr char ListSeparator = ':';

// filepath.ErrBadPattern: a malformed pattern for Match and Glob.
extern const error ErrBadPattern;

// filepath.SkipDir and filepath.SkipAll: fs::SkipDir and fs::SkipAll.
inline const error& SkipDir = fs::SkipDir;
inline const error& SkipAll = fs::SkipAll;

// filepath.Clean: the shortest equivalent path (path::Clean).
inline string Clean(std::string_view path) { return path::Clean(path); }
// filepath.Split: the directory, up to and including the last separator,
// and the file name.
inline std::pair<string, string> Split(std::string_view path) { return path::Split(path); }
// filepath.Join: the non-empty elements joined with separators, then
// cleaned; "" if all are empty.
inline string Join(std::span<const std::string_view> elem) { return path::Join(elem); }
inline string Join(std::initializer_list<std::string_view> elem) { return path::Join(elem); }
template <typename... S>
    requires(sizeof...(S) > 0 && (std::is_convertible_v<const S&, std::string_view> && ...))
string Join(const S&... elem) {
    return path::Join({std::string_view(elem)...});
}
// filepath.Ext: the extension, from the last dot of the last element.
inline string Ext(std::string_view path) { return path::Ext(path); }
// filepath.Base: the last element; "." for "", "/" for separators only.
inline string Base(std::string_view path) { return path::Base(path); }
// filepath.Dir: all but the last element, cleaned.
inline string Dir(std::string_view path) { return path::Dir(path); }
// filepath.IsAbs: whether the path starts with a separator.
inline bool IsAbs(std::string_view path) { return path::IsAbs(path); }
// filepath.VolumeName: the leading volume name; "" on Linux.
inline string VolumeName(std::string_view path) { return {}; }
// filepath.ToSlash and filepath.FromSlash: path itself on Linux.
inline string ToSlash(std::string_view path) { return string(path); }
inline string FromSlash(std::string_view path) { return string(path); }

// filepath.IsLocal: whether path is lexically within the directory it's
// evaluated in: not absolute, not empty, and without ".." leading out of
// it ("a/../b" is local, "../a" isn't).
bool IsLocal(std::string_view path);
// filepath.Localize: the operating system's path of a slash-separated,
// fs::ValidPath one; an error if it has no such path (it holds a NUL).
std::pair<string, error> Localize(std::string_view path);
// filepath.SplitList: the paths of a list such as $PATH; empty for "".
slice<string> SplitList(std::string_view path);
// filepath.Match: whether name matches the shell pattern (as path::Match);
// ErrBadPattern for a malformed pattern.
std::pair<bool, error> Match(std::string_view pattern, std::string_view name);
// filepath.Glob: the files that match pattern, in lexical order per
// directory; ErrBadPattern for a malformed pattern. I/O errors are
// ignored.
std::pair<std::vector<string>, error> Glob(std::string_view pattern);
// filepath.Abs: an absolute path for path, joined to the working directory
// if it's relative.
std::pair<string, error> Abs(std::string_view path);
// filepath.Rel: a relative path that joined to basepath is lexically
// targpath; an error if there is none.
std::pair<string, error> Rel(std::string_view basepath, std::string_view targpath);
// filepath.EvalSymlinks: path with its symbolic links resolved, cleaned.
std::pair<string, error> EvalSymlinks(std::string_view path);

// filepath.WalkFunc: called by Walk for each file or directory, with its
// Lstat FileInfo, or with the error of reading a directory or of the
// root's Lstat (then info is nil).
using WalkFunc = std::function<error(std::string_view path, const ptr<fs::FileInfo>& info, error err)>;

// filepath.WalkDir: walks the tree at root in lexical order, calling fn
// for each file and directory. It doesn't follow symbolic links.
error WalkDir(std::string_view root, const fs::WalkDirFunc& fn);
// filepath.Walk: as WalkDir, with a FileInfo for each file; WalkDir, which
// doesn't Lstat every file, is more efficient.
error Walk(std::string_view root, const WalkFunc& fn);

}  // namespace gopp::filepath
