// Internals of fork and exec, for the os package.
#pragma once

namespace gopp::syscall::detail {

// A system call without errno: the result, or -errno.
long raw_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) noexcept;

// Whether clone(CLONE_PIDFD) gives a pidfd (Go's checkClonePidfd): makes
// a child that exits at once.
bool check_clone_pidfd();

}  // namespace gopp::syscall::detail
