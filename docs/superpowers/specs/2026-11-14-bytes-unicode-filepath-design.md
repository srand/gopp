# bytes, unicode, unicode/utf8 and path/filepath

## Intent

Programs that handle text or walk the file system reach for `bytes`,
`unicode`, `unicode/utf8` and `path/filepath` all the time. go++ has only
an internal piece of utf8 and an ASCII-only `strings`. This adds Go's four
packages and makes `strings` Unicode-correct and complete alongside
`bytes`.

## Goals

- `unicode/utf8`: Go's whole package (`DecodeRune`, `DecodeRuneInString`,
  `DecodeLastRune*`, `EncodeRune`, `AppendRune`, `FullRune*`, `RuneCount*`,
  `RuneLen`, `RuneStart`, `Valid*` and the constants).
- `unicode`: Go's whole package, with Go 1.27.3's tables (Unicode 17.0.0):
  the categories, scripts and properties as `RangeTable`s, `Categories`,
  `Scripts`, `Properties`, `FoldCategory`, `FoldScript`, `CaseRanges`,
  `SpecialCase` (`TurkishCase`, `AzeriCase`), and `Is`, `In`, `IsOneOf`,
  `IsLetter`, `IsUpper`, `IsSpace`, ..., `To`, `ToUpper`, `ToLower`,
  `ToTitle` and `SimpleFold`.
- `bytes`: Go's package over `slice<byte>`: the search, split, trim, case
  and replace functions, the iterators (`Lines`, `SplitSeq`,
  `SplitAfterSeq`, `FieldsSeq`, `FieldsFuncSeq`), `Buffer` and `Reader`.
- `strings`: the same function set as `bytes`, and Unicode case mapping,
  folding and white space where it was ASCII-only.
- `path/filepath`: Go's package for Linux: the lexical functions, `Abs`,
  `Rel`, `Glob`, `EvalSymlinks`, `Walk`, `WalkDir`, `IsLocal`, `Localize`,
  `SplitList` and the separators.
- Go's tests, ported with their names.

## Non-goals

- `strings.Replacer`, `unicode/utf16` and the deprecated `Title`
  functions.
- `io.ByteReader`, `io.RuneScanner` and the other single-byte and rune
  interfaces. `Buffer` and `Reader` have the methods, not the interfaces.
- Windows and Plan 9 paths.

## Decided

- **The `gopp::bytes` type alias goes away.** `gopp::bytes` is now the
  package, so `bytes::Buffer` and `bytes::Contains` read as in Go. Byte
  slices are spelled `slice<byte>`. `to_bytes`, `as_bytes`,
  `as_string_view` and `String` stay where they are.
- The headers follow Go's paths: `<gopp/bytes.hpp>`, `<gopp/unicode.hpp>`,
  `<gopp/unicode/utf8.hpp>` (namespace `gopp::utf8`) and
  `<gopp/path/filepath.hpp>` (namespace `gopp::filepath`).
- **utf8** is ported from Go's `utf8.go` and is header-only and
  `constexpr`. It replaces `src/internal/utf8`. Each function has Go's two
  forms: `std::span<const byte>` for `[]byte` and `std::string_view` for
  `string`.
- **unicode's tables are generated.** `src/unicode/generate.py` reads Go's
  `tables.go` and writes `src/unicode/tables.cpp`, plus the declarations
  between markers in `<gopp/unicode.hpp>`. Every table is
  constant-initialized, so it's usable from any static initializer.
  `unicode::Letter` and the others are `const RangeTable*`, as Go's are
  `*RangeTable`. `Categories` and the other maps are sorted, constant
  arrays with `operator[]`: a missing name gives `nullptr`, as Go's map
  gives nil.
- **bytes**: functions that only read take `std::span<const byte>`, so
  they also accept arrays and vectors. Functions that return parts of
  their input (`Trim*`, `Split*`, `Fields*`, `Cut*`) take and return
  `slice<byte>`, and the parts share the input's array, as Go's
  subslices do. Where Go returns nil, they return a nil slice.
- **One implementation for both packages.** The algorithms work on
  `std::string_view` in an internal header. `strings` wraps them into
  `gopp::string`, sharing the input's buffer where Go shares it, and
  `bytes` wraps them into `slice<byte>`.
- Callbacks (`IndexFunc`, `FieldsFunc`, `TrimFunc`, `Map`, ...) are
  `std::function_ref`, so the functions live in .cpp files.
- The iterators are `std::generator`s, the C++ form of Go's `iter.Seq`.
- **filepath** works on Linux paths, which are slash-separated, so `Clean`,
  `Join`, `Match` and the other lexical functions use `path`'s. `Walk` and
  `WalkDir` are ported from Go's, over `os::Lstat` and `os::ReadDir`, and
  share `fs::SkipDir`, `fs::SkipAll` and `fs::WalkDirFunc`. `Glob` returns
  `std::vector<string>`, as `fs::Glob` does. `EvalSymlinks` is ported from
  Go's `symlink.go`.
