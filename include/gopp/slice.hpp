// gopp::slice<T>: Go's slice.
//
// A slice is a window (pointer, length, capacity) into a reference-counted
// backing array. Copying and re-slicing share the array, and append writes
// into it while capacity remains, exactly as in Go.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/string.hpp>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <memory>
#include <new>
#include <span>
#include <utility>
#endif

namespace gopp {

namespace detail {

template <typename T>
struct slice_array {
    std::atomic<unsigned> refs{1};
    std::size_t cap;

    T* data() noexcept { return reinterpret_cast<T*>(reinterpret_cast<char*>(this) + offset()); }

    static constexpr std::size_t offset() noexcept {
        return (sizeof(slice_array) + alignof(T) - 1) / alignof(T) * alignof(T);
    }

    // Allocates cap elements, all value-initialized (Go's zero value).
    static slice_array* alloc(std::size_t cap) {
        void* mem = ::operator new(offset() + cap * sizeof(T), std::align_val_t(std::max(alignof(T), alignof(slice_array))));
        auto* a = new (mem) slice_array();
        a->cap = cap;
        std::uninitialized_value_construct_n(a->data(), cap);
        return a;
    }

    void add_ref() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }

    void release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::destroy_n(data(), cap);
            this->~slice_array();
            ::operator delete(this, std::align_val_t(std::max(alignof(T), alignof(slice_array))));
        }
    }
};

}  // namespace detail

template <typename T>
class slice {
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;

    constexpr slice() noexcept = default;  // nil slice

    slice(std::initializer_list<T> il) : slice(make(il.size())) { std::copy(il.begin(), il.end(), p_); }

    slice(const slice& o) noexcept : a_(o.a_), p_(o.p_), n_(o.n_), cap_(o.cap_) {
        if (a_) a_->add_ref();
    }

    slice(slice&& o) noexcept : a_(o.a_), p_(o.p_), n_(o.n_), cap_(o.cap_) {
        o.a_ = nullptr;
        o.p_ = nullptr;
        o.n_ = o.cap_ = 0;
    }

    ~slice() {
        if (a_) a_->release();
    }

    slice& operator=(const slice& o) noexcept {
        slice(o).swap(*this);
        return *this;
    }

    slice& operator=(slice&& o) noexcept {
        slice(std::move(o)).swap(*this);
        return *this;
    }

    void swap(slice& o) noexcept {
        std::swap(a_, o.a_);
        std::swap(p_, o.p_);
        std::swap(n_, o.n_);
        std::swap(cap_, o.cap_);
    }

    // make([]T, len, cap)
    static slice make(size_type len, size_type cap) {
        if (len > cap) panic("makeslice: cap out of range");
        slice s;
        if (cap == 0) {
            // Empty but not nil: all such slices share one address, like
            // Go's runtime.zerobase.
            s.p_ = zero_base();
            return s;
        }
        s.a_ = detail::slice_array<T>::alloc(cap);
        s.p_ = s.a_->data();
        s.n_ = len;
        s.cap_ = cap;
        return s;
    }
    static slice make(size_type len) { return make(len, len); }

    // A slice holding a copy of the elements.
    static slice of(std::span<const T> v) {
        slice s = make(v.size());
        std::copy(v.begin(), v.end(), s.p_);
        return s;
    }

    T* data() const noexcept { return p_; }
    size_type size() const noexcept { return n_; }
    size_type len() const noexcept { return n_; }
    size_type cap() const noexcept { return cap_; }
    bool empty() const noexcept { return n_ == 0; }
    bool is_nil() const noexcept { return p_ == nullptr; }
    T* begin() const noexcept { return p_; }
    T* end() const noexcept { return p_ + n_; }

    T& operator[](size_type i) const {
        if (i >= n_) panic("index out of range");
        return p_[i];
    }

    // s[i:j], sharing the array. j may extend up to cap, as in Go.
    slice Sub(size_type i, size_type j) const {
        if (i > j || j > cap_) panic("slice bounds out of range");
        slice r(*this);
        r.p_ = p_ + i;
        r.n_ = j - i;
        r.cap_ = cap_ - i;
        return r;
    }
    slice Sub(size_type i) const { return Sub(i, n_); }
    // s[i:j:k]: also limits the capacity to k - i.
    slice Sub(size_type i, size_type j, size_type k) const {
        if (i > j || j > k || k > cap_) panic("slice bounds out of range");
        slice r(*this);
        r.p_ = p_ + i;
        r.n_ = j - i;
        r.cap_ = k - i;
        return r;
    }

    operator std::span<T>() const noexcept { return {p_, n_}; }
    operator std::span<const T>() const noexcept { return {p_, n_}; }
    std::span<T> span() const noexcept { return {p_, n_}; }

private:
    template <typename U, typename... Args>
    friend slice<U> append(slice<U> s, Args&&... args);

    static T* zero_base() noexcept {
        alignas(alignof(T) > 16 ? alignof(T) : 16) static unsigned char base[1];
        return reinterpret_cast<T*>(base);
    }

    detail::slice_array<T>* a_ = nullptr;
    T* p_ = nullptr;
    size_type n_ = 0;
    size_type cap_ = 0;
};

// append(s, elems...): writes into s's array while it has room, otherwise
// copies into a new array with room to grow.
template <typename T, typename... Args>
slice<T> append(slice<T> s, Args&&... args) {
    constexpr std::size_t k = sizeof...(Args);
    if (s.n_ + k > s.cap_) {
        std::size_t newcap = s.cap_ < 256 ? std::max<std::size_t>(s.cap_ * 2, 4) : s.cap_ + s.cap_ / 4 + 192;
        newcap = std::max(newcap, s.n_ + k);
        slice<T> t = slice<T>::make(s.n_, newcap);
        std::copy(s.p_, s.p_ + s.n_, t.p_);
        s = std::move(t);
    }
    T* p = s.p_ + s.n_;
    ((*p++ = std::forward<Args>(args)), ...);
    s.n_ += k;
    return s;
}

// append(s, v...) for a span of elements.
template <typename T>
slice<T> append_span(slice<T> s, std::span<const T> v) {
    if (s.len() + v.size() > s.cap()) {
        std::size_t newcap = std::max(s.cap() < 256 ? s.cap() * 2 : s.cap() + s.cap() / 4 + 192, s.len() + v.size());
        slice<T> t = slice<T>::make(s.len(), newcap);
        std::copy(s.begin(), s.end(), t.begin());
        s = std::move(t);
    }
    slice<T> r = s.Sub(0, s.len() + v.size());
    std::copy(v.begin(), v.end(), r.begin() + s.len());
    return r;
}

// copy(dst, src)
template <typename T>
std::size_t Copy(std::span<T> dst, std::span<const T> src) {
    std::size_t n = std::min(dst.size(), src.size());
    std::copy_n(src.begin(), n, dst.begin());
    return n;
}

using bytes = slice<byte>;

// Byte views of strings and back.
inline std::span<const byte> as_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const byte*>(s.data()), s.size()};
}

inline std::string_view as_string_view(std::span<const byte> b) noexcept {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// string(b): copies the bytes.
inline string String(std::span<const byte> b) { return string(as_string_view(b)); }

// []byte(s): copies the bytes.
inline bytes to_bytes(std::string_view s) { return bytes::of(as_bytes(s)); }

}  // namespace gopp
