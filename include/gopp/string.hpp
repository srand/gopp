// gopp::string: Go's immutable string.
//
// A string is a view of bytes kept alive by a shared, reference-counted
// buffer. Copying and slicing a string never copies its bytes. Strings made
// from literals with the _s suffix refer to static storage and allocate
// nothing.
#pragma once

#include <gopp/base.hpp>

#include <atomic>
#include <compare>
#include <cstring>
#include <format>
#include <functional>
#include <new>
#include <ostream>
#include <string>
#include <string_view>

namespace gopp {

namespace detail {

// Shared buffer of a string: a header followed by the bytes.
struct string_buf {
    std::atomic<unsigned> refs;
    std::size_t cap;

    char* data() noexcept { return reinterpret_cast<char*>(this + 1); }

    static string_buf* alloc(std::size_t n) {
        void* mem = ::operator new(sizeof(string_buf) + n);
        auto* b = new (mem) string_buf{{1}, n};
        return b;
    }

    void add_ref() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }

    void release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            this->~string_buf();
            ::operator delete(this);
        }
    }
};

}  // namespace detail

class string {
public:
    using size_type = std::size_t;
    using value_type = char;
    using const_iterator = const char*;
    static constexpr size_type npos = std::string_view::npos;

    constexpr string() noexcept = default;

    // Copies the bytes.
    string(std::string_view s) : string(copy_of(s)) {}
    string(const char* s) : string(std::string_view(s)) {}
    string(const std::string& s) : string(std::string_view(s)) {}
    string(const char* p, size_type n) : string(std::string_view(p, n)) {}

    string(const string& o) noexcept : buf_(o.buf_), p_(o.p_), n_(o.n_) {
        if (buf_) buf_->add_ref();
    }

    string(string&& o) noexcept : buf_(o.buf_), p_(o.p_), n_(o.n_) {
        o.buf_ = nullptr;
        o.p_ = nullptr;
        o.n_ = 0;
    }

    ~string() {
        if (buf_) buf_->release();
    }

    string& operator=(const string& o) noexcept {
        string(o).swap(*this);
        return *this;
    }

    string& operator=(string&& o) noexcept {
        string(std::move(o)).swap(*this);
        return *this;
    }

    void swap(string& o) noexcept {
        std::swap(buf_, o.buf_);
        std::swap(p_, o.p_);
        std::swap(n_, o.n_);
    }

    // A string that refers to static storage, such as a literal.
    static constexpr string from_static(std::string_view s) noexcept {
        string r;
        r.p_ = s.data();
        r.n_ = s.size();
        return r;
    }

    // Allocates an uninitialized buffer of n bytes and lets f fill it.
    template <typename F>
    static string build(size_type n, F&& f) {
        if (n == 0) return {};
        auto* b = detail::string_buf::alloc(n);
        f(b->data());
        string r;
        r.buf_ = b;
        r.p_ = b->data();
        r.n_ = n;
        return r;
    }

    constexpr const char* data() const noexcept { return p_; }
    constexpr size_type size() const noexcept { return n_; }
    constexpr size_type len() const noexcept { return n_; }
    constexpr bool empty() const noexcept { return n_ == 0; }
    constexpr const char* begin() const noexcept { return p_; }
    constexpr const char* end() const noexcept { return p_ + n_; }
    constexpr char operator[](size_type i) const noexcept { return p_[i]; }

    constexpr std::string_view view() const noexcept { return {p_, n_}; }
    constexpr operator std::string_view() const noexcept { return view(); }
    std::string str() const { return std::string(view()); }

    // s[i:j], sharing the buffer. Out-of-range indexes panic.
    string slice(size_type i, size_type j) const {
        if (i > j || j > n_) panic("slice bounds out of range");
        string r(*this);
        r.p_ = p_ + i;
        r.n_ = j - i;
        return r;
    }
    string slice(size_type i) const { return slice(i, n_); }

    // A substring that shares the buffer, given as a view into this string.
    string Sub(std::string_view v) const {
        return slice(static_cast<size_type>(v.data() - p_), static_cast<size_type>(v.data() - p_) + v.size());
    }

    size_type find(std::string_view s, size_type pos = 0) const noexcept { return view().find(s, pos); }
    size_type find(char c, size_type pos = 0) const noexcept { return view().find(c, pos); }
    size_type rfind(char c, size_type pos = npos) const noexcept { return view().rfind(c, pos); }
    bool starts_with(std::string_view s) const noexcept { return view().starts_with(s); }
    bool ends_with(std::string_view s) const noexcept { return view().ends_with(s); }

    friend bool operator==(const string& a, const string& b) noexcept { return a.view() == b.view(); }
    friend bool operator==(const string& a, std::string_view b) noexcept { return a.view() == b; }
    friend bool operator==(const string& a, const char* b) noexcept { return a.view() == b; }
    friend bool operator==(const string& a, const std::string& b) noexcept { return a.view() == std::string_view(b); }
    friend std::strong_ordering operator<=>(const string& a, const string& b) noexcept { return a.view() <=> b.view(); }
    friend std::strong_ordering operator<=>(const string& a, std::string_view b) noexcept { return a.view() <=> b; }
    friend std::strong_ordering operator<=>(const string& a, const std::string& b) noexcept {
        return a.view() <=> std::string_view(b);
    }

    static string concat(std::string_view a, std::string_view b) {
        return build(a.size() + b.size(), [&](char* p) {
            std::memcpy(p, a.data(), a.size());
            std::memcpy(p + a.size(), b.data(), b.size());
        });
    }
    friend string operator+(const string& a, const string& b) { return concat(a, b); }
    friend string operator+(const string& a, std::string_view b) { return concat(a, b); }
    friend string operator+(std::string_view a, const string& b) { return concat(a, b); }
    friend string operator+(const string& a, const char* b) { return concat(a, b); }
    friend string operator+(const char* a, const string& b) { return concat(a, b); }

    friend std::ostream& operator<<(std::ostream& os, const string& s) { return os << s.view(); }

private:
    static string copy_of(std::string_view s) {
        return build(s.size(), [&](char* p) { std::memcpy(p, s.data(), s.size()); });
    }

    detail::string_buf* buf_ = nullptr;
    const char* p_ = nullptr;
    size_type n_ = 0;
};

inline namespace literals {
// "text"_s: a string in static storage, without allocation.
constexpr string operator""_s(const char* p, std::size_t n) noexcept {
    return string::from_static(std::string_view(p, n));
}
}  // namespace literals

}  // namespace gopp

template <>
struct std::hash<gopp::string> {
    using is_transparent = void;
    size_t operator()(const gopp::string& s) const noexcept { return std::hash<std::string_view>()(s.view()); }
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>()(s); }
};

template <>
struct std::formatter<gopp::string> : std::formatter<std::string_view> {
    auto format(const gopp::string& s, auto& ctx) const { return std::formatter<std::string_view>::format(s.view(), ctx); }
};
