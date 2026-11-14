// The maps package: functions over maps of any key and value types.
//
//     for (auto& k : slices::Sorted(maps::Keys(m))) { ... }   // keys in order
//     auto m2 = maps::Clone(m);
//     maps::DeleteFunc(m, [](auto& k, auto& v) { return v == 0; });
//
// A map is std::map, std::unordered_map, or any type with their find,
// insert_or_assign and erase. Go's iterators (iter.Seq) are C++ ranges:
// All, Keys and Values return views, and Insert and Collect take any range
// of (key, value) pairs, such as slices::All's.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>

#include <concepts>
#include <functional>
#include <map>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#endif

namespace gopp::maps {

namespace detail {

template <typename M>
concept mapLike = requires(M& m, const typename M::key_type& k) {
    typename M::key_type;
    typename M::mapped_type;
    { m.find(k) } -> std::same_as<typename M::iterator>;
    m.erase(m.begin());
    m.size();
};

template <typename K>
concept hashable = requires(const K& k) {
    { std::hash<K>{}(k) } -> std::convertible_to<std::size_t>;
};

template <typename R>
using keyOf = std::remove_cvref_t<std::tuple_element_t<0, std::ranges::range_value_t<R>>>;
template <typename R>
using valueOf = std::remove_cvref_t<std::tuple_element_t<1, std::ranges::range_value_t<R>>>;

// The map Collect makes: an unordered_map if the key is hashable, as Go's
// map is, else a std::map.
template <typename K, typename V>
using mapFor = std::conditional_t<hashable<K>, std::unordered_map<K, V>, std::map<K, V>>;

}  // namespace detail

// maps.Equal: the same keys with equal values.
template <detail::mapLike M1, detail::mapLike M2>
bool Equal(const M1& m1, const M2& m2) {
    if (m1.size() != m2.size()) return false;
    for (const auto& [k, v1] : m1) {
        auto it = m2.find(k);
        if (it == m2.end() || !(v1 == it->second)) return false;
    }
    return true;
}

// maps.EqualFunc: the same keys, with values that eq says are equal.
template <detail::mapLike M1, detail::mapLike M2, typename F>
bool EqualFunc(const M1& m1, const M2& m2, F eq) {
    if (m1.size() != m2.size()) return false;
    for (const auto& [k, v1] : m1) {
        auto it = m2.find(k);
        if (it == m2.end() || !eq(v1, it->second)) return false;
    }
    return true;
}

// maps.Clone: a shallow copy.
template <detail::mapLike M>
M Clone(const M& m) {
    return m;
}

// maps.Copy: sets each key of src in dst, overwriting existing values.
template <detail::mapLike M1, detail::mapLike M2>
void Copy(M1& dst, const M2& src) {
    for (const auto& [k, v] : src) dst.insert_or_assign(k, v);
}

// maps.DeleteFunc: deletes the entries del(key, value) is true for.
template <detail::mapLike M, typename F>
void DeleteFunc(M& m, F del) {
    for (auto it = m.begin(); it != m.end();) {
        if (del(it->first, it->second)) {
            it = m.erase(it);
        } else {
            ++it;
        }
    }
}

// maps.All: (key, value) pairs.
template <std::ranges::viewable_range M>
    requires detail::mapLike<std::remove_cvref_t<M>>
auto All(M&& m) {
    return std::views::all(std::forward<M>(m));
}

// maps.Keys: the keys.
template <std::ranges::viewable_range M>
    requires detail::mapLike<std::remove_cvref_t<M>>
auto Keys(M&& m) {
    return std::views::keys(std::forward<M>(m));
}

// maps.Values: the values.
template <std::ranges::viewable_range M>
    requires detail::mapLike<std::remove_cvref_t<M>>
auto Values(M&& m) {
    return std::views::values(std::forward<M>(m));
}

// maps.Insert: sets the (key, value) pairs of seq in m.
template <detail::mapLike M, std::ranges::input_range R>
void Insert(M& m, R&& seq) {
    for (auto&& kv : seq) {
        auto&& [k, v] = kv;
        m.insert_or_assign(k, v);
    }
}

// maps.Collect: a new map of the (key, value) pairs of seq.
template <std::ranges::input_range R, typename M = detail::mapFor<detail::keyOf<R>, detail::valueOf<R>>>
M Collect(R&& seq) {
    M m;
    Insert(m, std::forward<R>(seq));
    return m;
}

}  // namespace gopp::maps
