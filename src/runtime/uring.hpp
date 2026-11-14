// io_uring for the system calls of files, which epoll can't wait for.
//
// A fiber submits its call to its thread's ring and parks until the
// completion comes in, while the thread runs other fibers: the call
// doesn't block a thread, as a blocking call does. A call that completes
// at once, as a read from the page cache usually does, doesn't park.
// Completions are taken by the submitting thread when it can, and by the
// network poller otherwise, which has each ring's descriptor in its epoll
// set.
//
// The rings are used if the kernel has io_uring and allows it (a sysctl or
// a seccomp filter may not), and GOPP_IOURING isn't 0; otherwise calls
// fall back to blocking calls. An operation the kernel doesn't support
// falls back on its own.
#pragma once

#include <gopp/runtime.hpp>

#include <cerrno>
#include <type_traits>

struct io_uring_sqe;

namespace gopp::runtime {

// Whether io_uring is in use and supports the operation (an IORING_OP_).
bool uringHas(int opcode) noexcept;

// Whether to make a call through io_uring: if it supports the operation
// and the calling fiber's P has other fibers to run while the call is in
// the kernel. With nothing else to run, a direct system call is faster:
// the kernel completes few calls at once (reads from the page cache of
// some file systems, opens of cached paths), and hands the others (statx,
// unlinkat, mkdirat, renameat, buffered writes, any read on tmpfs, ...)
// to a worker thread, whose wakeup, and then the poller's, costs more
// than most such calls.
bool uringWorth(int opcode) noexcept;

// Submits one request, which prep(sqe, arg) fills in, and waits for its
// completion: parks a fiber, blocks a thread that isn't one. Returns the
// result: >= 0, or -errno.
int uringCall(void (*prep)(io_uring_sqe*, void*), void* arg) noexcept;

// uring_call with a function object: prep(sqe).
template <typename F>
int uring(F&& prep) {
    return uringCall([](io_uring_sqe* sqe, void* arg) { (*static_cast<std::remove_reference_t<F>*>(arg))(sqe); },
                      &prep);
}

// Runs a system call through io_uring if it supports opcode, otherwise as
// a blocking call: call() makes the system call, returning -1 and setting
// errno on failure. Retries after EINTR. Returns the result, or -errno.
template <typename Prep, typename Call>
long uringOrBlocking(int opcode, Prep&& prep, Call&& call) {
    if (uringWorth(opcode)) {
        for (;;) {
            int r = uring(prep);
            if (r != -EINTR) return r;
        }
    }
    for (;;) {
        long r;
        int e;
        {
            blockingCall b;
            r = static_cast<long>(call());
            e = errno;
        }
        if (r >= 0) return r;
        if (e != EINTR) return -e;
    }
}

}  // namespace gopp::runtime
