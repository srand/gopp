// The cmp package: comparing ordered values.
//
//     cmp::Compare(a, b)                   // -1, 0 or +1
//     cmp::Or(flag_port, env_port, "8080"_s)   // the first non-empty one
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>

#include <concepts>
#include <type_traits>
#endif

namespace gopp::cmp {

// cmp.Ordered: types with <, <=, > and >=. Go allows integers, floats and
// strings; here any totally ordered type (strings, std::pair, a struct
// with <=>) is ordered too.
template <typename T>
concept Ordered = std::totally_ordered<T>;

namespace detail {
template <typename T>
constexpr bool isNan(const T& x) noexcept {
    if constexpr (std::is_floating_point_v<T>) {
        return x != x;
    } else {
        return false;
    }
}
}  // namespace detail

// cmp.Less: whether x < y, with NaN less than any other float.
template <Ordered T>
constexpr bool Less(const T& x, const T& y) noexcept {
    return (detail::isNan(x) && !detail::isNan(y)) || x < y;
}

// cmp.Compare: -1 if x < y, 0 if x == y, +1 if x > y. A NaN is less than
// any other float and equal to another NaN.
template <Ordered T>
constexpr int Compare(const T& x, const T& y) noexcept {
    bool x_nan = detail::isNan(x), y_nan = detail::isNan(y);
    if (x_nan) return y_nan ? 0 : -1;
    if (y_nan) return +1;
    if (x < y) return -1;
    if (x > y) return +1;
    return 0;
}

// cmp.Or: the first argument that isn't the zero value, or the zero value.
template <typename T, typename... U>
    requires(std::equality_comparable<T> && (std::convertible_to<U, T> && ...))
constexpr T Or(const T& first, const U&... rest) {
    const T zero{};
    if (first != zero) return first;
    T r = zero;
    (void)((T(rest) != zero ? (r = T(rest), true) : false) || ...);
    return r;
}

}  // namespace gopp::cmp
