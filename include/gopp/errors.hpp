// Go's error interface and the errors package.
//
// An error is a value: a nil error means success. Functions return errors
// instead of throwing:
//
//     if (auto err = srv.ListenAndServe()) { fmt::Println("{}", err); }
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>

#include <format>
#include <initializer_list>
#include <ostream>
#include <span>
#include <utility>
#include <vector>
#endif

namespace gopp {

class error;

// The error interface. Implementations derive from it and return their
// message from error(). Wrapping errors also implement unwrap().
struct error_interface : public virtual object {
    virtual string Error() const = 0;
    virtual const error_interface* Unwrap() const noexcept { return nullptr; }
    // Go's Unwrap() []error: the errors a joined error, or an Errorf with
    // several fmt::W arguments, wraps. errors::Is and As search them too.
    virtual std::span<const error> UnwrapList() const noexcept;
    // Optional: whether the error is a timeout (net.Error).
    virtual bool Timeout() const noexcept { return false; }
    // Optional: whether the error matches target for errors.Is, as an
    // Is(error) bool method does in Go; syscall::Errno matches
    // fs::ErrNotExist, for example.
    virtual bool Is(const error_interface* target) const noexcept { return false; }
};

// A value of Go's error type: nil, or a pointer to an implementation.
class error {
public:
    constexpr error() noexcept = default;
    constexpr error(std::nullptr_t) noexcept {}
    error(ptr<const error_interface> p) noexcept : p_(std::move(p)) {}

    template <typename E>
        requires std::is_base_of_v<error_interface, E>
    error(ptr<E> p) noexcept : p_(std::move(p)) {}

    explicit operator bool() const noexcept { return static_cast<bool>(p_); }
    bool is_nil() const noexcept { return !p_; }

    // err.Error(); "<nil>" for a nil error, as fmt prints it.
    string Error() const { return p_ ? p_->Error() : string::from_static("<nil>"); }

    const error_interface* operator->() const noexcept { return p_.get(); }
    const error_interface* get() const noexcept { return p_.get(); }

    // Identity comparison, as Go compares interface values holding pointers.
    friend bool operator==(const error& a, const error& b) noexcept { return a.p_.get() == b.p_.get(); }
    friend bool operator==(const error& a, std::nullptr_t) noexcept { return !a.p_; }

    friend std::ostream& operator<<(std::ostream& os, const error& e) { return os << e.Error(); }

private:
    ptr<const error_interface> p_;
};

namespace errors {

// errors.New
error New(string text);

// errors.Is: whether err or any error it wraps is target.
bool Is(const error& err, const error& target) noexcept;

namespace detail {
// The first error in the tree at e, depth first, that match says yes to.
const error_interface* find(const error_interface* e, bool (*match)(const error_interface*, void*), void* ctx) noexcept;
}  // namespace detail

// errors.As: the first error in err's tree (its Unwrap chain, and the
// lists of UnwrapList) that is an E, or nullptr.
template <typename E>
const E* As(const error& err) noexcept {
    auto* e = detail::find(
        err.get(), [](const error_interface* e, void*) { return dynamic_cast<const E*>(e) != nullptr; }, nullptr);
    return e ? dynamic_cast<const E*>(e) : nullptr;
}

// errors.Join: an error wrapping the non-nil errs, whose message is
// theirs joined by newlines; nil if there are none.
error Join(std::span<const error> errs);
error Join(std::initializer_list<error> errs);

// errors.Unwrap
error Unwrap(const error& err) noexcept;

}  // namespace errors

}  // namespace gopp

template <>
struct std::formatter<gopp::error> : std::formatter<std::string_view> {
    auto format(const gopp::error& e, auto& ctx) const {
        return std::formatter<std::string_view>::format(e.Error().view(), ctx);
    }
};
