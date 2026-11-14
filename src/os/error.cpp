// The errors of the os package, after Go's os/error.go.
#include "internal.hpp"

#include <string>

namespace gopp::os {

namespace {

struct timeoutError final : errorInterface {
    string Error() const override { return string::fromStatic("i/o timeout"); }
    bool Timeout() const noexcept override { return true; }
};

// Go's underlyingError: the error of a PathError, LinkError or
// SyscallError, or err.
const errorInterface* underlyingError(const error& err) noexcept {
    const errorInterface* e = err.get();
    if (auto* pe = dynamic_cast<const PathError*>(e)) return pe->Err.get();
    if (auto* le = dynamic_cast<const LinkError*>(e)) return le->Err.get();
    if (auto* se = dynamic_cast<const SyscallError*>(e)) return se->Err.get();
    return e;
}

bool underlyingErrorIs(const error& err, const error& target) noexcept {
    const errorInterface* e = underlyingError(err);
    if (e == target.get()) return true;
    auto* errnoErr = dynamic_cast<const syscall::Errno*>(e);
    return errnoErr && errnoErr->Is(target.get());
}

}  // namespace

const error ErrNoDeadline = errors::New("file type does not support deadline");
const error ErrDeadlineExceeded = error(make_ptr<timeoutError>());

string LinkError::Error() const {
    std::string s(Op.view());
    s += ' ';
    s += Old.view();
    s += ' ';
    s += New.view();
    s += ": ";
    s += Err.Error().view();
    return string(s);
}

string SyscallError::Error() const {
    std::string s(Syscall.view());
    s += ": ";
    s += Err.Error().view();
    return string(s);
}

gopp::error NewSyscallError(string syscall, gopp::error err) {
    if (!err) return nullptr;
    return make_ptr<SyscallError>(std::move(syscall), std::move(err));
}

bool IsExist(const gopp::error& err) noexcept { return underlyingErrorIs(err, ErrExist); }
bool IsNotExist(const gopp::error& err) noexcept { return underlyingErrorIs(err, ErrNotExist); }
bool IsPermission(const gopp::error& err) noexcept { return underlyingErrorIs(err, ErrPermission); }

bool IsTimeout(const gopp::error& err) noexcept {
    const errorInterface* e = underlyingError(err);
    return e && e->Timeout();
}

}  // namespace gopp::os
