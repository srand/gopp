# sort, slices, maps and cmp

## Intent

Sorting and searching come up in nearly every program, and go++ has none of
Go's helpers for them yet. Go code that calls `slices.Sort`,
`slices.BinarySearch`, `maps.Keys` or `sort.Slice` should port line by line.

## Goals

- `cmp`: `Ordered`, `Compare`, `Less`, `Or`.
- `slices`: the whole package. That covers the in-place functions (`Sort`, `SortFunc`,
  `SortStableFunc`, `IsSorted*`, `BinarySearch*`, `Index*`, `Contains*`,
  `Equal*`, `Compare*`, `Min*`, `Max*`, `Reverse`). It also covers the functions that
  return a new slice header (`Insert`, `Delete`, `DeleteFunc`, `Replace`,
  `Compact`, `CompactFunc`, `Clone`, `Grow`, `Clip`, `Concat`, `Repeat`)
  and the iterator functions (`All`, `Values`, `Backward`, `Collect`,
  `AppendSeq`, `Sorted`, `SortedFunc`, `SortedStableFunc`, `Chunk`).
- `maps`: `Equal`, `EqualFunc`, `Clone`, `Copy`, `DeleteFunc`, `All`, `Keys`,
  `Values`, `Insert`, `Collect`.
- `sort`: `Interface`, `Sort`, `Stable`, `IsSorted`, `Reverse`, `Slice`,
  `SliceStable`, `SliceIsSorted`, `Search`, `Find`, `Ints`, `Strings`,
  `Float64s`, the `*AreSorted` and `Search*` helpers, and `IntSlice`,
  `StringSlice` and `Float64Slice`.
- The algorithms are Go's own: pattern-defeating quicksort for the unstable
  sorts, and insertion sort plus SymMerge for the stable ones. Sorts then
  make the same comparisons and produce the same orders as Go.
- Go's `sort`, `slices`, `maps` and `cmp` tests, ported with their names.

## Non-goals

- `sort`'s and `slices`' benchmarks.

## Decided

- Go's `iter.Seq` maps to C++ ranges. `slices::Values(s)` is a view.
  `maps::Keys(m)` is `std::views::keys`. `slices::Collect` and `Sorted`
  accept any input range, so `slices::Sorted(maps::Keys(m))` works as it
  does in Go. `All` yields `(index, value)` and `(key, value)` pairs.
- The in-place functions accept any contiguous range: `gopp::slice`,
  `std::vector`, `std::array` and `std::span`.
- The functions that change the length take and return `gopp::slice<T>`, as
  Go's do. Like Go 1.22, they zero the elements they leave behind.
- The map functions accept `std::map` and `std::unordered_map` (any type with
  `find`, `insert_or_assign` and `erase`). `maps::Collect` builds a
  `std::unordered_map` by default.
- One template implements each algorithm, parameterized by `less(i, j)` and
  `swap(i, j)`. `sort::Sort` (virtual `Interface`), `sort::Slice`,
  `slices::Sort` and `slices::SortFunc` all instantiate it.
- `sort::Interface` is a plain abstract class, not a `gopp::object`. It is
  only ever passed by reference.
- Go's `int` is `isize`. `sort::Ints` takes `std::span<isize>`.
- The headers are `<gopp/cmp.hpp>`, `<gopp/slices.hpp>`, `<gopp/maps.hpp>`
  and `<gopp/sort.hpp>`. The algorithms live in headers. The non-template
  parts of `sort` live in `src/sort/sort.cpp`.
