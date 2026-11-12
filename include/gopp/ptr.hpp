// Intrusive reference counting, derived from fstree's intrusive_ptr.
//
// Objects shared by pointer, as Go shares pointers and interface values,
// derive from gopp::object and are held by gopp::ptr<T>. The count lives in
// the object, so a ptr is one machine word and an object one allocation.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <string>
#include <vector>
#include <atomic>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>
#endif

namespace gopp {

#ifdef GOPP_DEBUG_LEAKS
// Leak checking (GOPP_DEBUG_LEAKS): every object is recorded while alive.
class object;
namespace detail {
void object_created(const object* o) noexcept;
void object_destroyed(const object* o) noexcept;
}  // namespace detail
// Objects alive.
long objects_in_use() noexcept;
// Objects created so far.
unsigned long objects_created() noexcept;
// The dynamic types of the objects alive that were created after the
// first `since`, with their counts ("gopp::http::Request x2").
std::vector<std::string> live_objects_since(unsigned long since);
#endif

// Base class of reference-counted objects. Interfaces derive from it
// virtually, so that a class implementing several interfaces has one count.
class object {
public:
    object() noexcept {
#ifdef GOPP_DEBUG_LEAKS
        detail::object_created(this);
#endif
    }

    // Copies and moves of an object start with a count of their own.
    object(const object&) noexcept : object() {}
    object& operator=(const object&) noexcept { return *this; }

    virtual ~object() {
#ifdef GOPP_DEBUG_LEAKS
        detail::object_destroyed(this);
#endif
    }

    void add_ref() const noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }

    void release_ref() const noexcept {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    unsigned use_count() const noexcept { return refs_.load(std::memory_order_relaxed); }

private:
    mutable std::atomic<unsigned> refs_{0};
};

template <typename T>
class intrusive_ptr {
public:
    using element_type = T;

    constexpr intrusive_ptr() noexcept = default;
    constexpr intrusive_ptr(std::nullptr_t) noexcept {}

    explicit intrusive_ptr(T* p) noexcept : p_(p) {
        if (p_) p_->add_ref();
    }

    intrusive_ptr(const intrusive_ptr& o) noexcept : p_(o.p_) {
        if (p_) p_->add_ref();
    }

    intrusive_ptr(intrusive_ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

    // Upcasts: ptr<ServeMux> converts to ptr<Handler>.
    template <typename U>
        requires std::is_convertible_v<U*, T*>
    intrusive_ptr(const intrusive_ptr<U>& o) noexcept : p_(o.get()) {
        if (p_) p_->add_ref();
    }

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    intrusive_ptr(intrusive_ptr<U>&& o) noexcept : p_(o.detach()) {}

    ~intrusive_ptr() {
        if (p_) p_->release_ref();
    }

    intrusive_ptr& operator=(const intrusive_ptr& o) noexcept {
        intrusive_ptr(o).swap(*this);
        return *this;
    }

    intrusive_ptr& operator=(intrusive_ptr&& o) noexcept {
        intrusive_ptr(std::move(o)).swap(*this);
        return *this;
    }

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    intrusive_ptr& operator=(intrusive_ptr<U> o) noexcept {
        intrusive_ptr(std::move(o)).swap(*this);
        return *this;
    }

    intrusive_ptr& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    void reset() noexcept { intrusive_ptr().swap(*this); }
    void swap(intrusive_ptr& o) noexcept { std::swap(p_, o.p_); }

    // Releases ownership without decrementing the count.
    T* detach() noexcept { return std::exchange(p_, nullptr); }

    // Takes ownership of a pointer whose count was already incremented.
    static intrusive_ptr adopt(T* p) noexcept {
        intrusive_ptr r;
        r.p_ = p;
        return r;
    }

    T* get() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    template <typename U>
    bool operator==(const intrusive_ptr<U>& o) const noexcept { return p_ == o.get(); }
    bool operator==(std::nullptr_t) const noexcept { return p_ == nullptr; }

private:
    T* p_ = nullptr;
};

template <typename T>
using ptr = intrusive_ptr<T>;

// Allocates a T, like Go's &T{...} or new(T).
template <typename T, typename... Args>
ptr<T> make_ptr(Args&&... args) {
    return ptr<T>(new T(std::forward<Args>(args)...));
}

// Interface type assertion: x.(T) with the comma-ok form, nil if it fails.
template <typename T, typename U>
ptr<T> dynamic_pointer_cast(const ptr<U>& p) noexcept {
    return ptr<T>(dynamic_cast<T*>(p.get()));
}

template <typename T, typename U>
ptr<T> static_pointer_cast(const ptr<U>& p) noexcept {
    return ptr<T>(static_cast<T*>(p.get()));
}

// Returns a ptr to an object that is already owned by another ptr.
template <typename T>
ptr<T> ptr_from_this(T* self) noexcept {
    return ptr<T>(self);
}

}  // namespace gopp

template <typename T>
struct std::hash<gopp::intrusive_ptr<T>> {
    size_t operator()(const gopp::intrusive_ptr<T>& p) const noexcept { return std::hash<T*>()(p.get()); }
};
