// The syscall package: the errors of system calls.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/string.hpp>
#endif

namespace gopp::syscall {

// syscall.Errno: an errno from a system call. errors::Is matches it with
// the fs errors: ENOENT is fs::ErrNotExist, EEXIST and ENOTEMPTY are
// fs::ErrExist, and EACCES and EPERM are fs::ErrPermission. Compare codes
// with errors::As<syscall::Errno>(err)->Code.
struct Errno : error_interface {
    int Code;
    explicit Errno(int c) : Code(c) {}
    string Error() const override;
    bool Timeout() const noexcept override;
    bool Is(const error_interface* target) const noexcept override;
};

// An Errno error for code.
gopp::error errno_error(int code);

}  // namespace gopp::syscall
