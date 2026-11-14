// The slices package: functions over slices of any element type.
//
//     slices::Sort(names);
//     auto [i, found] = slices::BinarySearch(names, "gopher"_s);
//     s = slices::Insert(s, 1, x, y);          // returns the new slice, as in Go
//     for (auto [i, v] : slices::All(s)) { ... }
//     auto keys = slices::Sorted(maps::Keys(m));
//
// The functions that work in place (sorting, searching, comparing) take any
// contiguous range: gopp::slice, std::vector, std::array or std::span. The
// ones that change the length (Insert, Delete, Compact, ...) take and return
// gopp::slice, sharing its array as Go's do, and zero the elements they
// leave behind. Go's iterators (iter.Seq) are C++ ranges: All, Values and
// Backward return views, and Collect, AppendSeq and Sorted take any input
// range.
//
// The sorts are Go's: pattern-defeating quicksort (pdqsort) for Sort and
// SortFunc, insertion sort and SymMerge for SortStableFunc, so they compare
// and move elements as Go's do.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/cmp.hpp>
#include <gopp/slice.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#endif

namespace gopp::slices {

namespace detail {

template <typename S>
concept contiguous = std::ranges::contiguous_range<S> && std::ranges::sized_range<S>;

template <typename S>
using elem_t = std::remove_reference_t<std::ranges::range_reference_t<S>>;

template <typename S>
std::span<elem_t<S>> spanOf(S& s) noexcept {
    return {std::ranges::data(s), std::ranges::size(s)};
}

// bits.Len(uint(n))
inline int bitsLen(isize n) noexcept { return std::bit_width(static_cast<std::size_t>(n)); }

// Go's copy: the ranges may overlap.
template <typename T>
void goCopy(T* dst, const T* src, std::size_t n) {
    if (dst == src || n == 0) return;
    if (dst < src) {
        std::copy(src, src + n, dst);
    } else {
        std::copy_backward(src, src + n, dst + n);
    }
}

// Whether the memory of a and b overlaps.
template <typename T>
bool overlaps(std::span<const T> a, std::span<const T> b) noexcept {
    if (a.empty() || b.empty()) return false;
    auto pa = reinterpret_cast<std::uintptr_t>(a.data()), pb = reinterpret_cast<std::uintptr_t>(b.data());
    return pa <= pb + (b.size() - 1) * sizeof(T) + (sizeof(T) - 1) &&
           pb <= pa + (a.size() - 1) * sizeof(T) + (sizeof(T) - 1);
}

// The index in haystack where needle, which lies within it, starts.
template <typename T>
std::size_t startIdx(std::span<const T> haystack, std::span<const T> needle) {
    for (std::size_t i = 0; i < haystack.size(); i++) {
        if (&haystack[i] == &needle[0]) return i;
    }
    panic("needle not found");
}

template <typename T>
void reverse(std::span<T> s) noexcept {
    for (std::size_t i = 0, j = s.size(); i + 1 < j; i++, j--) {
        using std::swap;
        swap(s[i], s[j - 1]);
    }
}

// s_final[i] = s_orig[i+r], wrapping around.
template <typename T>
void rotateLeft(std::span<T> s, std::size_t r) {
    reverse(s.first(r));
    reverse(s.subspan(r));
    reverse(s);
}

template <typename T>
void rotateRight(std::span<T> s, std::size_t r) {
    rotateLeft(s, s.size() - r);
}

// ---------------------------------------------------------------------------
// Go's sorting algorithms (sort/zsortinterface.go), over data with
// less(i, j) and swap(i, j).

enum class sorted_hint { unknown, increasing, decreasing };

struct xorshift {
    uint64 r;
    uint64 next() noexcept {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        return r;
    }
};

inline std::size_t nextPowerOfTwo(isize length) noexcept {
    return std::size_t(1) << std::bit_width(static_cast<std::size_t>(length));
}

// insertionSort sorts data[a:b].
template <typename D>
void insertion_sort(D& data, isize a, isize b) {
    for (isize i = a + 1; i < b; i++) {
        for (isize j = i; j > a && data.less(j, j - 1); j--) {
            data.swap(j, j - 1);
        }
    }
}

// siftDown implements the heap property on data[lo:hi]; first is the
// offset of the root.
template <typename D>
void sift_down(D& data, isize lo, isize hi, isize first) {
    isize root = lo;
    for (;;) {
        isize child = 2 * root + 1;
        if (child >= hi) break;
        if (child + 1 < hi && data.less(first + child, first + child + 1)) child++;
        if (!data.less(first + root, first + child)) return;
        data.swap(first + root, first + child);
        root = child;
    }
}

template <typename D>
void heapSort(D& data, isize a, isize b) {
    isize first = a, lo = 0, hi = b - a;
    for (isize i = (hi - 1) / 2; i >= 0; i--) sift_down(data, i, hi, first);
    for (isize i = hi - 1; i >= 0; i--) {
        data.swap(first, first + i);
        sift_down(data, lo, i, first);
    }
}

template <typename D>
std::pair<isize, bool> partition(D& data, isize a, isize b, isize pivot) {
    data.swap(a, pivot);
    isize i = a + 1, j = b - 1;
    while (i <= j && data.less(i, a)) i++;
    while (i <= j && !data.less(j, a)) j--;
    if (i > j) {
        data.swap(j, a);
        return {j, true};
    }
    data.swap(i, j);
    i++;
    j--;
    for (;;) {
        while (i <= j && data.less(i, a)) i++;
        while (i <= j && !data.less(j, a)) j--;
        if (i > j) break;
        data.swap(i, j);
        i++;
        j--;
    }
    data.swap(j, a);
    return {j, false};
}

template <typename D>
isize partitionEqual(D& data, isize a, isize b, isize pivot) {
    data.swap(a, pivot);
    isize i = a + 1, j = b - 1;
    for (;;) {
        while (i <= j && !data.less(a, i)) i++;
        while (i <= j && data.less(a, j)) j--;
        if (i > j) break;
        data.swap(i, j);
        i++;
        j--;
    }
    return i;
}

template <typename D>
bool partialInsertionSort(D& data, isize a, isize b) {
    constexpr int max_steps = 5;
    constexpr isize shortest_shifting = 50;
    isize i = a + 1;
    for (int step = 0; step < max_steps; step++) {
        while (i < b && !data.less(i, i - 1)) i++;
        if (i == b) return true;
        if (b - a < shortest_shifting) return false;
        data.swap(i, i - 1);
        if (i - a >= 2) {
            for (isize j = i - 1; j >= 1; j--) {
                if (!data.less(j, j - 1)) break;
                data.swap(j, j - 1);
            }
        }
        if (b - i >= 2) {
            for (isize j = i + 1; j < b; j++) {
                if (!data.less(j, j - 1)) break;
                data.swap(j, j - 1);
            }
        }
    }
    return false;
}

template <typename D>
void break_patterns(D& data, isize a, isize b) {
    isize length = b - a;
    if (length >= 8) {
        xorshift random{static_cast<uint64>(length)};
        std::size_t modulus = nextPowerOfTwo(length);
        for (isize idx = a + (length / 4) * 2 - 1; idx <= a + (length / 4) * 2 + 1; idx++) {
            auto other = static_cast<isize>(static_cast<std::size_t>(random.next()) & (modulus - 1));
            if (other >= length) other -= length;
            data.swap(idx, a + other);
        }
    }
}

template <typename D>
std::pair<isize, isize> order2(D& data, isize a, isize b, int& swaps) {
    if (data.less(b, a)) {
        swaps++;
        return {b, a};
    }
    return {a, b};
}

template <typename D>
isize median(D& data, isize a, isize b, isize c, int& swaps) {
    std::tie(a, b) = order2(data, a, b, swaps);
    std::tie(b, c) = order2(data, b, c, swaps);
    std::tie(a, b) = order2(data, a, b, swaps);
    return b;
}

template <typename D>
isize medianAdjacent(D& data, isize a, int& swaps) {
    return median(data, a - 1, a, a + 1, swaps);
}

template <typename D>
std::pair<isize, sorted_hint> choosePivot(D& data, isize a, isize b) {
    constexpr isize shortest_ninther = 50;
    constexpr int max_swaps = 4 * 3;
    isize l = b - a;
    int swaps = 0;
    isize i = a + l / 4 * 1, j = a + l / 4 * 2, k = a + l / 4 * 3;
    if (l >= 8) {
        if (l >= shortest_ninther) {
            i = medianAdjacent(data, i, swaps);
            j = medianAdjacent(data, j, swaps);
            k = medianAdjacent(data, k, swaps);
        }
        j = median(data, i, j, k, swaps);
    }
    switch (swaps) {
        case 0: return {j, sorted_hint::increasing};
        case max_swaps: return {j, sorted_hint::decreasing};
        default: return {j, sorted_hint::unknown};
    }
}

template <typename D>
void reverse_range(D& data, isize a, isize b) {
    for (isize i = a, j = b - 1; i < j; i++, j--) data.swap(i, j);
}

// pdqsort sorts data[a:b]; limit is the number of bad pivots allowed
// before falling back to heapsort.
template <typename D>
void pdqsort(D& data, isize a, isize b, int limit) {
    constexpr isize max_insertion = 12;
    bool was_balanced = true, was_partitioned = true;
    for (;;) {
        isize length = b - a;
        if (length <= max_insertion) {
            insertion_sort(data, a, b);
            return;
        }
        if (limit == 0) {
            heapSort(data, a, b);
            return;
        }
        if (!was_balanced) {
            break_patterns(data, a, b);
            limit--;
        }
        auto [pivot, hint] = choosePivot(data, a, b);
        if (hint == sorted_hint::decreasing) {
            reverse_range(data, a, b);
            pivot = (b - 1) - (pivot - a);
            hint = sorted_hint::increasing;
        }
        if (was_balanced && was_partitioned && hint == sorted_hint::increasing) {
            if (partialInsertionSort(data, a, b)) return;
        }
        if (a > 0 && !data.less(a - 1, pivot)) {
            a = partitionEqual(data, a, b, pivot);
            continue;
        }
        auto [mid, already_partitioned] = partition(data, a, b, pivot);
        was_partitioned = already_partitioned;
        isize left_len = mid - a, right_len = b - mid;
        isize balance_threshold = length / 8;
        if (left_len < right_len) {
            was_balanced = left_len >= balance_threshold;
            pdqsort(data, a, mid, limit);
            a = mid + 1;
        } else {
            was_balanced = right_len >= balance_threshold;
            pdqsort(data, mid + 1, b, limit);
            b = mid;
        }
    }
}

template <typename D>
void swapRange(D& data, isize a, isize b, isize n) {
    for (isize i = 0; i < n; i++) data.swap(a + i, b + i);
}

// Rotates data[a:m] and data[m:b]: 'x u v y' becomes 'x v u y'.
template <typename D>
void rotate(D& data, isize a, isize m, isize b) {
    isize i = m - a, j = b - m;
    while (i != j) {
        if (i > j) {
            swapRange(data, m - i, m, j);
            i -= j;
        } else {
            swapRange(data, m - i, m + j - i, i);
            j -= i;
        }
    }
    swapRange(data, m - i, m, i);
}

// symMerge merges the sorted data[a:m] and data[m:b] (Kim and Kutzner's
// SymMerge); a < m && m < b.
template <typename D>
void sym_merge(D& data, isize a, isize m, isize b) {
    if (m - a == 1) {
        isize i = m, j = b;
        while (i < j) {
            auto h = static_cast<isize>(static_cast<std::size_t>(i + j) >> 1);
            if (data.less(h, a)) {
                i = h + 1;
            } else {
                j = h;
            }
        }
        for (isize k = a; k < i - 1; k++) data.swap(k, k + 1);
        return;
    }
    if (b - m == 1) {
        isize i = a, j = m;
        while (i < j) {
            auto h = static_cast<isize>(static_cast<std::size_t>(i + j) >> 1);
            if (!data.less(m, h)) {
                i = h + 1;
            } else {
                j = h;
            }
        }
        for (isize k = m; k > i; k--) data.swap(k, k - 1);
        return;
    }
    auto mid = static_cast<isize>(static_cast<std::size_t>(a + b) >> 1);
    isize n = mid + m;
    isize start, r;
    if (m > mid) {
        start = n - b;
        r = mid;
    } else {
        start = a;
        r = m;
    }
    isize p = n - 1;
    while (start < r) {
        auto c = static_cast<isize>(static_cast<std::size_t>(start + r) >> 1);
        if (!data.less(p - c, c)) {
            start = c + 1;
        } else {
            r = c;
        }
    }
    isize end = n - start;
    if (start < m && m < end) rotate(data, start, m, end);
    if (a < start && start < mid) sym_merge(data, a, start, mid);
    if (mid < end && end < b) sym_merge(data, mid, end, b);
}

template <typename D>
void stable(D& data, isize n) {
    isize block_size = 20;
    isize a = 0, b = block_size;
    while (b <= n) {
        insertion_sort(data, a, b);
        a = b;
        b += block_size;
    }
    insertion_sort(data, a, n);
    while (block_size < n) {
        a = 0;
        b = 2 * block_size;
        while (b <= n) {
            sym_merge(data, a, a + block_size, b);
            a = b;
            b += 2 * block_size;
        }
        if (isize m = a + block_size; m < n) sym_merge(data, a, m, n);
        block_size *= 2;
    }
}

// Sorting data over elements, ordered by less(x, y).
template <typename T, typename Less>
struct elements {
    T* p;
    Less less_fn;
    bool less(isize i, isize j) { return less_fn(p[i], p[j]); }
    void swap(isize i, isize j) {
        using std::swap;
        swap(p[i], p[j]);
    }
};

template <typename T, typename Less>
elements<T, Less> elementsOf(std::span<T> s, Less less) {
    return {s.data(), std::move(less)};
}

struct ordered_less {
    template <typename T>
    bool operator()(const T& x, const T& y) const {
        return cmp::Less(x, y);
    }
};

template <typename F>
struct cmp_less {
    F& cmp;
    template <typename T>
    bool operator()(const T& x, const T& y) const {
        return cmp(x, y) < 0;
    }
};

// append(s, v...), where v may lie in s's array beyond its length.
template <typename T>
slice<T> appendOverlapping(slice<T> s, std::span<const T> v) {
    std::size_t n = s.len();
    if (n + v.size() > s.cap()) return appendSpan(std::move(s), v);
    s = s.Sub(0, n + v.size());
    goCopy(s.data() + n, v.data(), v.size());
    return s;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Comparing and searching

// slices.Equal: the same length and equal elements. NaNs aren't equal.
template <detail::contiguous S1, detail::contiguous S2>
bool Equal(const S1& s1, const S2& s2) {
    auto a = std::span(std::ranges::data(s1), std::ranges::size(s1));
    auto b = std::span(std::ranges::data(s2), std::ranges::size(s2));
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (!(a[i] == b[i])) return false;
    }
    return true;
}

// slices.EqualFunc
template <detail::contiguous S1, detail::contiguous S2, typename F>
bool EqualFunc(const S1& s1, const S2& s2, F eq) {
    auto a = std::span(std::ranges::data(s1), std::ranges::size(s1));
    auto b = std::span(std::ranges::data(s2), std::ranges::size(s2));
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (!eq(a[i], b[i])) return false;
    }
    return true;
}

// slices.Compare: the first non-zero cmp::Compare of the elements; a
// shorter prefix is less.
template <detail::contiguous S1, detail::contiguous S2>
int Compare(const S1& s1, const S2& s2) {
    auto a = std::span(std::ranges::data(s1), std::ranges::size(s1));
    auto b = std::span(std::ranges::data(s2), std::ranges::size(s2));
    for (std::size_t i = 0; i < a.size(); i++) {
        if (i >= b.size()) return +1;
        if (int c = cmp::Compare(a[i], b[i]); c != 0) return c;
    }
    return a.size() < b.size() ? -1 : 0;
}

// slices.CompareFunc
template <detail::contiguous S1, detail::contiguous S2, typename F>
int CompareFunc(const S1& s1, const S2& s2, F cmp) {
    auto a = std::span(std::ranges::data(s1), std::ranges::size(s1));
    auto b = std::span(std::ranges::data(s2), std::ranges::size(s2));
    for (std::size_t i = 0; i < a.size(); i++) {
        if (i >= b.size()) return +1;
        if (int c = cmp(a[i], b[i]); c != 0) return c;
    }
    return a.size() < b.size() ? -1 : 0;
}

// slices.Index: the index of the first v in s, or -1.
template <detail::contiguous S, typename V>
isize Index(const S& s, const V& v) {
    auto a = std::span(std::ranges::data(s), std::ranges::size(s));
    for (std::size_t i = 0; i < a.size(); i++) {
        if (v == a[i]) return static_cast<isize>(i);
    }
    return -1;
}

// slices.IndexFunc: the index of the first element f is true for, or -1.
template <detail::contiguous S, typename F>
isize IndexFunc(const S& s, F f) {
    auto a = std::span(std::ranges::data(s), std::ranges::size(s));
    for (std::size_t i = 0; i < a.size(); i++) {
        if (f(a[i])) return static_cast<isize>(i);
    }
    return -1;
}

// slices.Contains
template <detail::contiguous S, typename V>
bool Contains(const S& s, const V& v) {
    return Index(s, v) >= 0;
}

// slices.ContainsFunc
template <detail::contiguous S, typename F>
bool ContainsFunc(const S& s, F f) {
    return IndexFunc(s, std::move(f)) >= 0;
}

// slices.Reverse: reverses s in place.
template <detail::contiguous S>
void Reverse(S&& s) {
    detail::reverse(detail::spanOf(s));
}

// ---------------------------------------------------------------------------
// Sorting

// slices.Sort: ascending, NaNs first.
template <detail::contiguous S>
    requires cmp::Ordered<detail::elem_t<S>>
void Sort(S&& x) {
    auto s = detail::spanOf(x);
    auto data = detail::elementsOf(s, detail::ordered_less{});
    auto n = static_cast<isize>(s.size());
    detail::pdqsort(data, 0, n, detail::bitsLen(n));
}

// slices.SortFunc: ascending by cmp(a, b), negative when a < b. Not stable.
template <detail::contiguous S, typename F>
void SortFunc(S&& x, F cmp) {
    auto s = detail::spanOf(x);
    auto data = detail::elementsOf(s, detail::cmp_less<F>{cmp});
    auto n = static_cast<isize>(s.size());
    detail::pdqsort(data, 0, n, detail::bitsLen(n));
}

// slices.SortStableFunc: as SortFunc, keeping equal elements in order.
template <detail::contiguous S, typename F>
void SortStableFunc(S&& x, F cmp) {
    auto s = detail::spanOf(x);
    auto data = detail::elementsOf(s, detail::cmp_less<F>{cmp});
    detail::stable(data, static_cast<isize>(s.size()));
}

// slices.IsSorted
template <detail::contiguous S>
    requires cmp::Ordered<detail::elem_t<S>>
bool IsSorted(const S& x) {
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    for (std::size_t i = s.size(); i > 1; i--) {
        if (cmp::Less(s[i - 1], s[i - 2])) return false;
    }
    return true;
}

// slices.IsSortedFunc
template <detail::contiguous S, typename F>
bool IsSortedFunc(const S& x, F cmp) {
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    for (std::size_t i = s.size(); i > 1; i--) {
        if (cmp(s[i - 1], s[i - 2]) < 0) return false;
    }
    return true;
}

// slices.Min: panics if x is empty; a NaN makes the result NaN.
template <detail::contiguous S>
    requires cmp::Ordered<detail::elem_t<S>>
std::remove_const_t<detail::elem_t<S>> Min(const S& x) {
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    if (s.empty()) panic("slices.Min: empty list");
    std::remove_const_t<detail::elem_t<S>> m = s[0];
    for (std::size_t i = 1; i < s.size(); i++) {
        if constexpr (std::is_floating_point_v<decltype(m)>) {
            if (m != m) return m;
            if (s[i] != s[i] || s[i] < m || (s[i] == 0 && m == 0 && std::signbit(s[i]))) m = s[i];
        } else {
            if (s[i] < m) m = s[i];
        }
    }
    return m;
}

// slices.MinFunc: the first minimal element; panics if x is empty.
template <detail::contiguous S, typename F>
std::remove_const_t<detail::elem_t<S>> MinFunc(const S& x, F cmp) {
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    if (s.empty()) panic("slices.MinFunc: empty list");
    std::size_t m = 0;
    for (std::size_t i = 1; i < s.size(); i++) {
        if (cmp(s[i], s[m]) < 0) m = i;
    }
    return s[m];
}

// slices.Max: panics if x is empty; a NaN makes the result NaN.
template <detail::contiguous S>
    requires cmp::Ordered<detail::elem_t<S>>
std::remove_const_t<detail::elem_t<S>> Max(const S& x) {
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    if (s.empty()) panic("slices.Max: empty list");
    std::remove_const_t<detail::elem_t<S>> m = s[0];
    for (std::size_t i = 1; i < s.size(); i++) {
        if constexpr (std::is_floating_point_v<decltype(m)>) {
            if (m != m) return m;
            if (s[i] != s[i] || s[i] > m || (s[i] == 0 && m == 0 && !std::signbit(s[i]))) m = s[i];
        } else {
            if (s[i] > m) m = s[i];
        }
    }
    return m;
}

// slices.MaxFunc: the first maximal element; panics if x is empty.
template <detail::contiguous S, typename F>
std::remove_const_t<detail::elem_t<S>> MaxFunc(const S& x, F cmp) {
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    if (s.empty()) panic("slices.MaxFunc: empty list");
    std::size_t m = 0;
    for (std::size_t i = 1; i < s.size(); i++) {
        if (cmp(s[i], s[m]) > 0) m = i;
    }
    return s[m];
}

// slices.BinarySearch: the first position of target in the sorted x, or
// where it would be inserted, and whether it's there.
template <detail::contiguous S, typename T>
    requires cmp::Ordered<std::remove_const_t<detail::elem_t<S>>>
std::pair<isize, bool> BinarySearch(const S& x, const T& t) {
    using E = std::remove_const_t<detail::elem_t<S>>;
    const E& target = t;
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    std::size_t i = 0, j = s.size();
    while (i < j) {
        std::size_t h = (i + j) >> 1;
        if (cmp::Less(s[h], target)) {
            i = h + 1;
        } else {
            j = h;
        }
    }
    bool found = i < s.size() && (s[i] == target || (cmp::detail::isNan(s[i]) && cmp::detail::isNan(target)));
    return {static_cast<isize>(i), found};
}

// slices.BinarySearchFunc: as BinarySearch, with cmp(element, target).
template <detail::contiguous S, typename T, typename F>
std::pair<isize, bool> BinarySearchFunc(const S& x, const T& target, F cmp) {
    auto s = std::span(std::ranges::data(x), std::ranges::size(x));
    std::size_t i = 0, j = s.size();
    while (i < j) {
        std::size_t h = (i + j) >> 1;
        if (cmp(s[h], target) < 0) {
            i = h + 1;
        } else {
            j = h;
        }
    }
    return {static_cast<isize>(i), i < s.size() && cmp(s[i], target) == 0};
}

// ---------------------------------------------------------------------------
// Functions that change the length: they take and return a gopp::slice.

// slices.Insert(s, i, v...) with v a span: inserts v at index i. v may be
// part of s.
template <typename T>
slice<T> Insert(slice<T> s, isize i, std::type_identity_t<std::span<const T>> v) {
    if (i < 0 || static_cast<std::size_t>(i) > s.len()) panic("slice bounds out of range");
    auto ui = static_cast<std::size_t>(i);
    std::size_t m = v.size();
    if (m == 0) return s;
    std::size_t n = s.len();
    if (ui == n) return detail::appendOverlapping(std::move(s), v);
    if (n + m > s.cap()) {
        slice<T> s2 = appendSpan(s.Sub(0, ui), std::span<const T>(slice<T>::make(n + m - ui)));
        std::copy(v.begin(), v.end(), s2.begin() + ui);
        std::copy(s.begin() + ui, s.end(), s2.begin() + ui + m);
        return s2;
    }
    s = s.Sub(0, n + m);
    if (!detail::overlaps<T>(v, s.Sub(ui + m).span())) {
        detail::goCopy(s.data() + ui + m, s.data() + ui, n - ui);
        detail::goCopy(s.data() + ui, v.data(), m);
        return s;
    }
    detail::goCopy(s.data() + n, v.data(), m);
    detail::rotateRight(s.Sub(ui).span(), m);
    return s;
}

// slices.Insert(s, i, v...): inserts the values v at index i.
template <typename T, typename... V>
    requires(std::convertible_to<V, T> && ...)
slice<T> Insert(slice<T> s, isize i, V&&... v) {
    std::array<T, sizeof...(V)> vs{T(std::forward<V>(v))...};
    return Insert(std::move(s), i, std::span<const T>(vs.data(), vs.size()));
}

// slices.Delete: removes s[i:j], zeroing the elements left at the end.
template <typename T>
slice<T> Delete(slice<T> s, isize i, isize j) {
    if (i < 0 || j < i || static_cast<std::size_t>(j) > s.len()) panic("slice bounds out of range");
    if (i == j) return s;
    auto ui = static_cast<std::size_t>(i), uj = static_cast<std::size_t>(j);
    std::size_t oldlen = s.len();
    detail::goCopy(s.data() + ui, s.data() + uj, oldlen - uj);
    std::size_t newlen = oldlen - (uj - ui);
    std::fill(s.data() + newlen, s.data() + oldlen, T{});
    return s.Sub(0, newlen);
}

// slices.DeleteFunc: removes the elements del is true for.
template <typename T, typename F>
slice<T> DeleteFunc(slice<T> s, F del) {
    isize first = IndexFunc(s, del);
    if (first == -1) return s;
    auto i = static_cast<std::size_t>(first);
    for (std::size_t j = i + 1; j < s.len(); j++) {
        if (T& v = s.data()[j]; !del(v)) {
            s.data()[i] = v;
            i++;
        }
    }
    std::fill(s.data() + i, s.data() + s.len(), T{});
    return s.Sub(0, i);
}

// slices.Replace(s, i, j, v...) with v a span: replaces s[i:j] with v.
template <typename T>
slice<T> Replace(slice<T> s, isize i, isize j, std::type_identity_t<std::span<const T>> v) {
    if (i < 0 || j < i || static_cast<std::size_t>(j) > s.len()) panic("slice bounds out of range");
    if (i == j) return Insert(std::move(s), i, v);
    auto ui = static_cast<std::size_t>(i), uj = static_cast<std::size_t>(j);
    if (uj == s.len()) {
        slice<T> s2 = detail::appendOverlapping(s.Sub(0, ui), v);
        if (s2.len() < s.len()) std::fill(s.data() + s2.len(), s.data() + s.len(), T{});
        return s2;
    }
    std::size_t tot = ui + v.size() + (s.len() - uj);
    if (tot > s.cap()) {
        slice<T> s2 = appendSpan(s.Sub(0, ui), std::span<const T>(slice<T>::make(tot - ui)));
        std::copy(v.begin(), v.end(), s2.begin() + ui);
        std::copy(s.begin() + uj, s.end(), s2.begin() + ui + v.size());
        return s2;
    }
    slice<T> r = s.Sub(0, tot);
    if (ui + v.size() <= uj) {
        detail::goCopy(r.data() + ui, v.data(), v.size());
        detail::goCopy(r.data() + ui + v.size(), s.data() + uj, s.len() - uj);
        if (tot < s.len()) std::fill(s.data() + tot, s.data() + s.len(), T{});
        return r;
    }
    if (!detail::overlaps<T>(r.Sub(ui + v.size()).span(), v)) {
        detail::goCopy(r.data() + ui + v.size(), s.data() + uj, s.len() - uj);
        detail::goCopy(r.data() + ui, v.data(), v.size());
        return r;
    }
    std::size_t y = v.size() - (uj - ui);
    if (!detail::overlaps<T>(r.Sub(ui, uj).span(), v)) {
        detail::goCopy(r.data() + ui, v.data() + y, uj - ui);
        detail::goCopy(r.data() + s.len(), v.data(), y);
        detail::rotateRight(r.Sub(ui).span(), y);
        return r;
    }
    if (!detail::overlaps<T>(r.Sub(s.len()).span(), v)) {
        detail::goCopy(r.data() + s.len(), v.data(), y);
        detail::goCopy(r.data() + ui, v.data() + y, uj - ui);
        detail::rotateRight(r.Sub(ui).span(), y);
        return r;
    }
    std::size_t k = detail::startIdx<T>(v, s.Sub(uj).span());
    detail::goCopy(r.data() + ui, v.data(), v.size());
    detail::goCopy(r.data() + ui + v.size(), r.data() + ui + k, tot - ui - v.size());
    return r;
}

// slices.Replace(s, i, j, v...): replaces s[i:j] with the values v.
template <typename T, typename... V>
    requires(std::convertible_to<V, T> && ...)
slice<T> Replace(slice<T> s, isize i, isize j, V&&... v) {
    std::array<T, sizeof...(V)> vs{T(std::forward<V>(v))...};
    return Replace(std::move(s), i, j, std::span<const T>(vs.data(), vs.size()));
}

// slices.Clone: a shallow copy; nil stays nil.
template <typename T>
slice<T> Clone(const slice<T>& s) {
    if (s.isNil()) return {};
    return slice<T>::of(s);
}

// slices.Compact: replaces runs of equal elements with one copy.
template <typename T>
slice<T> Compact(slice<T> s) {
    if (s.len() < 2) return s;
    T* p = s.data();
    for (std::size_t k = 1; k < s.len(); k++) {
        if (p[k] == p[k - 1]) {
            T* p2 = p + k;
            std::size_t n2 = s.len() - k;
            for (std::size_t k2 = 1; k2 < n2; k2++) {
                if (!(p2[k2] == p2[k2 - 1])) {
                    p[k] = p2[k2];
                    k++;
                }
            }
            std::fill(p + k, p + s.len(), T{});
            return s.Sub(0, k);
        }
    }
    return s;
}

// slices.CompactFunc: as Compact with an equality function, keeping the
// first of each run.
template <typename T, typename F>
slice<T> CompactFunc(slice<T> s, F eq) {
    if (s.len() < 2) return s;
    T* p = s.data();
    for (std::size_t k = 1; k < s.len(); k++) {
        if (eq(p[k], p[k - 1])) {
            T* p2 = p + k;
            std::size_t n2 = s.len() - k;
            for (std::size_t k2 = 1; k2 < n2; k2++) {
                if (!eq(p2[k2], p2[k2 - 1])) {
                    p[k] = p2[k2];
                    k++;
                }
            }
            std::fill(p + k, p + s.len(), T{});
            return s.Sub(0, k);
        }
    }
    return s;
}

// slices.Grow: makes room to append n more elements without allocating.
template <typename T>
slice<T> Grow(slice<T> s, isize n) {
    if (n < 0) panic("cannot be negative");
    auto un = static_cast<std::size_t>(n);
    if (un > s.cap() - s.len()) {
        un -= s.cap() - s.len();
        std::size_t len = s.len();
        s = appendSpan(s.Sub(0, s.cap()), std::span<const T>(slice<T>::make(un))).Sub(0, len);
    }
    return s;
}

// slices.Clip: s[:len(s):len(s)].
template <typename T>
slice<T> Clip(const slice<T>& s) {
    return s.Sub(0, s.len(), s.len());
}

// slices.Concat: a new slice of the slices' elements; nil if empty.
template <typename T, typename... S>
    requires(std::same_as<S, slice<T>> && ...)
slice<T> Concat(const slice<T>& first, const S&... rest) {
    std::size_t size = first.len() + (rest.len() + ... + 0);
    slice<T> r = Grow(slice<T>{}, static_cast<isize>(size));
    r = appendSpan(r, std::span<const T>(first));
    ((r = appendSpan(r, std::span<const T>(rest))), ...);
    return r;
}

// slices.Repeat: x repeated count times; never nil.
template <detail::contiguous S>
slice<std::remove_const_t<detail::elem_t<S>>> Repeat(const S& x, isize count) {
    using T = std::remove_const_t<detail::elem_t<S>>;
    if (count < 0) panic("cannot be negative");
    std::size_t len = std::ranges::size(x);
    auto ucount = static_cast<std::size_t>(count);
    if (len != 0 && ucount > static_cast<std::size_t>(std::numeric_limits<isize>::max()) / len) {
        panic("the result of (len(x) * count) overflows");
    }
    auto r = slice<T>::make(len * ucount);
    for (std::size_t n = 0; n < r.len(); n += len) {
        std::copy_n(std::ranges::data(x), len, r.data() + n);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Iterators: Go's iter.Seq as ranges.

// slices.All: (index, value) pairs.
template <std::ranges::viewable_range S>
    requires detail::contiguous<S>
auto All(S&& s) {
    return std::views::enumerate(std::forward<S>(s));
}

// slices.Backward: (index, value) pairs, from the last.
template <std::ranges::viewable_range S>
    requires detail::contiguous<S>
auto Backward(S&& s) {
    return std::views::enumerate(std::forward<S>(s)) | std::views::reverse;
}

// slices.Values: the elements.
template <std::ranges::viewable_range S>
    requires detail::contiguous<S>
auto Values(S&& s) {
    return std::views::all(std::forward<S>(s));
}

// slices.AppendSeq: appends the values of seq to s.
template <typename T, std::ranges::input_range R>
slice<T> AppendSeq(slice<T> s, R&& seq) {
    for (auto&& v : seq) s = append(std::move(s), T(std::forward<decltype(v)>(v)));
    return s;
}

// slices.Collect: the values of seq in a new slice; nil if there are none.
template <std::ranges::input_range R>
slice<std::ranges::range_value_t<R>> Collect(R&& seq) {
    return AppendSeq(slice<std::ranges::range_value_t<R>>{}, std::forward<R>(seq));
}

// slices.Sorted: Collect, then Sort.
template <std::ranges::input_range R>
slice<std::ranges::range_value_t<R>> Sorted(R&& seq) {
    auto s = Collect(std::forward<R>(seq));
    Sort(s);
    return s;
}

// slices.SortedFunc: Collect, then SortFunc.
template <std::ranges::input_range R, typename F>
slice<std::ranges::range_value_t<R>> SortedFunc(R&& seq, F cmp) {
    auto s = Collect(std::forward<R>(seq));
    SortFunc(s, std::move(cmp));
    return s;
}

// slices.SortedStableFunc: Collect, then SortStableFunc.
template <std::ranges::input_range R, typename F>
slice<std::ranges::range_value_t<R>> SortedStableFunc(R&& seq, F cmp) {
    auto s = Collect(std::forward<R>(seq));
    SortStableFunc(s, std::move(cmp));
    return s;
}

// slices.Chunk: consecutive sub-slices of up to n elements, sharing s's
// array, each clipped to its length. Panics if n < 1.
template <typename T>
auto Chunk(slice<T> s, isize n) {
    if (n < 1) panic("cannot be less than 1");
    auto un = static_cast<std::size_t>(n);
    std::size_t count = (s.len() + un - 1) / un;
    return std::views::iota(std::size_t(0), count) | std::views::transform([s = std::move(s), un](std::size_t c) {
               std::size_t i = c * un, end = std::min(i + un, s.len());
               return s.Sub(i, end, end);
           });
}

// slices.Chunk over other contiguous ranges: spans of up to n elements.
template <detail::contiguous S>
auto Chunk(S& s, isize n) {
    if (n < 1) panic("cannot be less than 1");
    auto sp = detail::spanOf(s);
    auto un = static_cast<std::size_t>(n);
    std::size_t count = (sp.size() + un - 1) / un;
    return std::views::iota(std::size_t(0), count) | std::views::transform([sp, un](std::size_t c) {
               std::size_t i = c * un;
               return sp.subspan(i, std::min(un, sp.size() - i));
           });
}

}  // namespace gopp::slices
