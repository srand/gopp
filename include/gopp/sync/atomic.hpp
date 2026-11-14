// The sync/atomic package: Go's atomic types and functions, after Go
// 1.27.3, over std::atomic. All operations are sequentially consistent,
// as Go's are.
//
//     atomic::Int64 hits;
//     hits.Add(1);
//     hits.Load();
//
//     atomic::Pointer<Config> cfg;          // a ptr<Config>, swapped atomically
//     cfg.Store(make_ptr<Config>(...));
//     ptr<Config> c = cfg.Load();
//
//     atomic::AddInt64(&counter, 1);         // on a plain int64
//
// The types aren't copyable, as Go's noCopy has them.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/ptr.hpp>

#include <atomic>
#include <memory>
#include <utility>
#endif

namespace gopp::atomic {

namespace detail {

// An integer type of the package: Load, Store, Swap, CompareAndSwap, Add,
// And and Or.
template <typename T>
class integer {
public:
    constexpr integer() noexcept = default;
    integer(const integer&) = delete;
    integer& operator=(const integer&) = delete;

    // The value.
    T Load() const noexcept { return v_.load(); }
    // Sets the value.
    void Store(T val) noexcept { v_.store(val); }
    // Sets the value and returns the old one.
    T Swap(T val) noexcept { return v_.exchange(val); }
    // Sets the value to val if it is old; whether it was.
    bool CompareAndSwap(T old, T val) noexcept { return v_.compare_exchange_strong(old, val); }
    // Adds delta (wrapping around) and returns the new value.
    T Add(T delta) noexcept { return T(v_.fetch_add(delta) + delta); }
    // Bitwise ands mask in and returns the old value.
    T And(T mask) noexcept { return v_.fetch_and(mask); }
    // Bitwise ors mask in and returns the old value.
    T Or(T mask) noexcept { return v_.fetch_or(mask); }

private:
    std::atomic<T> v_{0};
};

}  // namespace detail

// atomic.Int32, Int64, Uint32, Uint64 and Uintptr.
using Int32 = detail::integer<int32>;
using Int64 = detail::integer<int64>;
using Uint32 = detail::integer<uint32>;
using Uint64 = detail::integer<uint64>;
using Uintptr = detail::integer<uintptr>;

// atomic.Bool
class Bool {
public:
    constexpr Bool() noexcept = default;
    Bool(const Bool&) = delete;
    Bool& operator=(const Bool&) = delete;

    bool Load() const noexcept { return v_.load(); }
    void Store(bool val) noexcept { v_.store(val); }
    bool Swap(bool val) noexcept { return v_.exchange(val); }
    bool CompareAndSwap(bool old, bool val) noexcept { return v_.compare_exchange_strong(old, val); }

private:
    std::atomic<bool> v_{false};
};

namespace detail {
// A lock word for Pointer: a reference count change and a swap can't be
// one atomic operation, so the few instructions that do both are under a
// spin lock.
class spinlock {
public:
    void lock() noexcept {
        while (f_.test_and_set(std::memory_order_acquire)) {
            while (f_.test(std::memory_order_relaxed)) {
#if defined(__x86_64__)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                asm volatile("yield");
#endif
            }
        }
    }
    void unlock() noexcept { f_.clear(std::memory_order_release); }

private:
    std::atomic_flag f_;
};
}  // namespace detail

// atomic.Pointer[T]: a ptr<T>, Go's *T, loaded and stored atomically.
template <typename T>
class Pointer {
public:
    constexpr Pointer() noexcept = default;
    Pointer(const Pointer&) = delete;
    Pointer& operator=(const Pointer&) = delete;

    ptr<T> Load() const noexcept {
        lock_.lock();
        ptr<T> p = p_;
        lock_.unlock();
        return p;
    }
    void Store(ptr<T> val) noexcept { Swap(std::move(val)); }
    ptr<T> Swap(ptr<T> val) noexcept {
        lock_.lock();
        p_.swap(val);
        lock_.unlock();
        return val;  // the old value, released here by the caller
    }
    // Sets the pointer to val if it points where old does (or both are
    // nil); whether it did.
    bool CompareAndSwap(const ptr<T>& old, ptr<T> val) noexcept {
        lock_.lock();
        bool same = p_.get() == old.get();
        if (same) p_.swap(val);
        lock_.unlock();
        return same;  // val, the old value if swapped, is released here
    }

private:
    mutable detail::spinlock lock_;
    ptr<T> p_;
};

// atomic.Value, typed: a value of T loaded and stored atomically, as Go
// code uses atomic.Value with values of one type. Load returns T's zero
// value until the first Store.
template <typename T>
class Value {
public:
    Value() noexcept = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    T Load() const {
        auto p = v_.load();
        return p ? *p : T{};
    }
    void Store(T val) { v_.store(std::make_shared<const T>(std::move(val))); }
    T Swap(T val) {
        auto old = v_.exchange(std::make_shared<const T>(std::move(val)));
        return old ? *old : T{};
    }
    // Stores val if the value is old (compared with ==; never stored counts
    // as T's zero value); whether it did.
    bool CompareAndSwap(const T& old, T val) {
        auto next = std::make_shared<const T>(std::move(val));
        auto cur = v_.load();
        for (;;) {
            if (!(cur ? *cur == old : T{} == old)) return false;
            if (v_.compare_exchange_weak(cur, next)) return true;
        }
    }

private:
    std::atomic<std::shared_ptr<const T>> v_;
};

// The functions on plain integers, as Go's take pointers: AddInt32,
// LoadInt64, SwapUint32, CompareAndSwapUintptr, AndInt64, OrUint32, ...
// The addresses must be aligned for their type (they are, unless packed).
#define GOPP_ATOMIC_FUNCS(Name, T)                                                               \
    inline T Add##Name(T* addr, T delta) noexcept { return T(std::atomic_ref<T>(*addr).fetch_add(delta) + delta); } \
    inline T Load##Name(const T* addr) noexcept { return std::atomic_ref<const T>(*addr).load(); }       \
    inline void Store##Name(T* addr, T val) noexcept { std::atomic_ref<T>(*addr).store(val); }         \
    inline T Swap##Name(T* addr, T val) noexcept { return std::atomic_ref<T>(*addr).exchange(val); }   \
    inline bool CompareAndSwap##Name(T* addr, T old, T val) noexcept {                          \
        return std::atomic_ref<T>(*addr).compare_exchange_strong(old, val);                     \
    }                                                                                            \
    inline T And##Name(T* addr, T mask) noexcept { return std::atomic_ref<T>(*addr).fetch_and(mask); } \
    inline T Or##Name(T* addr, T mask) noexcept { return std::atomic_ref<T>(*addr).fetch_or(mask); }

GOPP_ATOMIC_FUNCS(Int32, int32)
GOPP_ATOMIC_FUNCS(Int64, int64)
GOPP_ATOMIC_FUNCS(Uint32, uint32)
GOPP_ATOMIC_FUNCS(Uint64, uint64)
GOPP_ATOMIC_FUNCS(Uintptr, uintptr)

#undef GOPP_ATOMIC_FUNCS

}  // namespace gopp::atomic
