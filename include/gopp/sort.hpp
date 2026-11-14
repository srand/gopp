// The sort package: sorting collections through an interface, and binary
// search.
//
//     struct by_age : sort::Interface {
//         std::span<person> p;
//         isize Len() override { return std::ssize(p); }
//         bool Less(isize i, isize j) override { return p[i].age < p[j].age; }
//         void Swap(isize i, isize j) override { std::swap(p[i], p[j]); }
//     };
//     sort::Sort(by_age{.p = people});
//     sort::Slice(people, [&](isize i, isize j) { return people[i].name < people[j].name; });
//     isize i = sort::Search(n, [&](isize i) { return data[i] >= 23; });
//
// As in Go, slices::Sort and slices::SortFunc are usually simpler. The
// algorithms are Go's (see <gopp/slices.hpp>).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/slices.hpp>
#include <gopp/string.hpp>

#include <span>
#include <utility>
#endif

namespace gopp::sort {

// sort.Interface: a collection that Sort can sort, by index.
struct Interface {
    virtual ~Interface() = default;
    // The number of elements.
    virtual isize Len() = 0;
    // Whether element i must sort before element j.
    virtual bool Less(isize i, isize j) = 0;
    // Swaps elements i and j.
    virtual void Swap(isize i, isize j) = 0;
};

// sort.Sort: ascending by Less. Not stable.
void Sort(Interface& data);
inline void Sort(Interface&& data) { Sort(data); }

// sort.Stable: ascending by Less, keeping equal elements in order.
void Stable(Interface& data);
inline void Stable(Interface&& data) { Stable(data); }

// sort.IsSorted
bool IsSorted(Interface& data);
inline bool IsSorted(Interface&& data) { return IsSorted(data); }

namespace detail {
class reverse final : public Interface {
public:
    explicit reverse(Interface& data) noexcept : data_(data) {}
    isize Len() override { return data_.Len(); }
    bool Less(isize i, isize j) override { return data_.Less(j, i); }
    void Swap(isize i, isize j) override { data_.Swap(i, j); }

private:
    Interface& data_;
};

// Interface's Less and Swap for the algorithms.
struct interfaceData {
    Interface& data;
    bool less(isize i, isize j) { return data.Less(i, j); }
    void swap(isize i, isize j) { data.Swap(i, j); }
};

// sort's lessSwap: Slice's less on indexes, swapping elements.
template <typename T, typename F>
struct lessSwap {
    T* p;
    F& lessFn;
    bool less(isize i, isize j) { return lessFn(i, j); }
    void swap(isize i, isize j) {
        using std::swap;
        swap(p[i], p[j]);
    }
};
}  // namespace detail

// sort.Reverse: data in the reverse order. It refers to data, which must
// outlive it (a temporary does, within the full expression).
inline detail::reverse Reverse(Interface& data) noexcept { return detail::reverse(data); }
inline detail::reverse Reverse(Interface&& data) noexcept { return detail::reverse(data); }

// sort.Slice: sorts x by less(i, j) on indexes. Not stable.
template <slices::detail::contiguous S, typename F>
void Slice(S&& x, F less) {
    auto s = slices::detail::spanOf(x);
    detail::lessSwap<slices::detail::elemT<S>, F> data{s.data(), less};
    auto n = static_cast<isize>(s.size());
    slices::detail::pdqsort(data, 0, n, slices::detail::bitsLen(n));
}

// sort.SliceStable: sorts x by less(i, j), keeping equal elements in order.
template <slices::detail::contiguous S, typename F>
void SliceStable(S&& x, F less) {
    auto s = slices::detail::spanOf(x);
    detail::lessSwap<slices::detail::elemT<S>, F> data{s.data(), less};
    slices::detail::stable(data, static_cast<isize>(s.size()));
}

// sort.SliceIsSorted
template <slices::detail::contiguous S, typename F>
bool SliceIsSorted(const S& x, F less) {
    for (auto i = static_cast<isize>(std::ranges::size(x)) - 1; i > 0; i--) {
        if (less(i, i - 1)) return false;
    }
    return true;
}

// sort.Search: the smallest i in [0, n) for which f(i) is true, given that
// f is false and then true; n if it never is.
template <typename F>
isize Search(isize n, F f) {
    isize i = 0, j = n;
    while (i < j) {
        auto h = static_cast<isize>(static_cast<std::size_t>(i + j) >> 1);
        if (!f(h)) {
            i = h + 1;
        } else {
            j = h;
        }
    }
    return i;
}

// sort.Find: the smallest i in [0, n) at which cmp(i) <= 0, given that
// cmp is positive, then zero, then negative; and whether cmp(i) == 0.
template <typename F>
std::pair<isize, bool> Find(isize n, F cmp) {
    isize i = 0, j = n;
    while (i < j) {
        auto h = static_cast<isize>(static_cast<std::size_t>(i + j) >> 1);
        if (cmp(h) > 0) {
            i = h + 1;
        } else {
            j = h;
        }
    }
    return {i, i < n && cmp(i) == 0};
}

// sort.Ints, Float64s (NaNs first) and Strings: ascending.
inline void Ints(std::span<isize> x) { slices::Sort(x); }
inline void Float64s(std::span<double> x) { slices::Sort(x); }
inline void Strings(std::span<string> x) { slices::Sort(x); }

inline bool IntsAreSorted(std::span<const isize> x) { return slices::IsSorted(x); }
inline bool Float64sAreSorted(std::span<const double> x) { return slices::IsSorted(x); }
inline bool StringsAreSorted(std::span<const string> x) { return slices::IsSorted(x); }

// sort.SearchInts, SearchFloat64s, SearchStrings: Search for the first
// element >= x in the sorted a.
inline isize SearchInts(std::span<const isize> a, isize x) {
    return Search(std::ssize(a), [&](isize i) { return a[i] >= x; });
}
inline isize SearchFloat64s(std::span<const double> a, double x) {
    return Search(std::ssize(a), [&](isize i) { return a[i] >= x; });
}
inline isize SearchStrings(std::span<const string> a, std::string_view x) {
    return Search(std::ssize(a), [&](isize i) { return a[i] >= x; });
}

// sort.IntSlice: Interface over a span of isize, ascending.
struct IntSlice : Interface {
    std::span<isize> s;
    IntSlice(std::span<isize> s) noexcept : s(s) {}
    isize Len() override { return std::ssize(s); }
    bool Less(isize i, isize j) override { return s[i] < s[j]; }
    void Swap(isize i, isize j) override { std::swap(s[i], s[j]); }
    void Sort() { sort::Sort(*this); }
    isize Search(isize x) const { return SearchInts(s, x); }
};

// sort.Float64Slice: Interface over a span of double, ascending, NaNs first.
struct Float64Slice : Interface {
    std::span<double> s;
    Float64Slice(std::span<double> s) noexcept : s(s) {}
    isize Len() override { return std::ssize(s); }
    bool Less(isize i, isize j) override { return s[i] < s[j] || (s[i] != s[i] && s[j] == s[j]); }
    void Swap(isize i, isize j) override { std::swap(s[i], s[j]); }
    void Sort() { sort::Sort(*this); }
    isize Search(double x) const { return SearchFloat64s(s, x); }
};

// sort.StringSlice: Interface over a span of string, ascending.
struct StringSlice : Interface {
    std::span<string> s;
    StringSlice(std::span<string> s) noexcept : s(s) {}
    isize Len() override { return std::ssize(s); }
    bool Less(isize i, isize j) override { return s[i] < s[j]; }
    void Swap(isize i, isize j) override { s[i].swap(s[j]); }
    void Sort() { sort::Sort(*this); }
    isize Search(std::string_view x) const { return SearchStrings(s, x); }
};

}  // namespace gopp::sort
