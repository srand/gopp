// A file descriptor of the os package, after Go's internal/poll.FD.
//
// A pollable descriptor (a pipe, a socket, a terminal) is in non-blocking
// mode and registered with the runtime's poller: an operation that would
// block parks its fiber until the descriptor is ready, it's closed, or a
// deadline passes. Any other descriptor (a regular file, a directory) is
// used in blocking mode, and its system calls are blocking calls: their
// thread hands its processor to another thread meanwhile.
#pragma once

#include "../../runtime/netpoll.hpp"

#include <gopp/errors.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <sys/stat.h>

#include <atomic>
#include <functional>
#include <span>
#include <utility>

namespace gopp::poll {

// The error of operations on a closed file: "use of closed file". The os
// package reports it as fs::ErrClosed.
extern const error ErrFileClosing;

// The struct stat of a statx.
void stat_from_statx(const struct statx& x, struct stat* st) noexcept;

// fstatat(2), through io_uring's statx if it can; returns 0 or the errno.
int statat(int dirfd, const char* name, struct stat* st, int flags) noexcept;

class FD {
public:
    explicit FD(int sysfd) noexcept : sysfd_(sysfd) {}
    // Closes the descriptor if Close wasn't called, as Go's finalizer does.
    ~FD();
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;

    // Registers a pollable descriptor, which must be in non-blocking mode,
    // with the poller. If it can't be, as epoll refuses regular files, the
    // descriptor stays blocking and the error is returned.
    error Init(bool pollable);

    // Closes the descriptor once no operation uses it; first wakes the
    // operations parked on it, which fail with ErrFileClosing. For a
    // pollable descriptor, waits until it's closed.
    error Close();

    // Puts the descriptor in blocking mode, for use outside the FD.
    error SetBlocking();

    // read(2) and friends, retried after EINTR. Read returns 0 and
    // io::EOF_ at the end of the file.
    std::pair<isize, error> Read(std::span<byte> p);
    std::pair<isize, error> Pread(std::span<byte> p, int64 off);
    std::pair<isize, error> Write(std::span<const byte> p);
    std::pair<isize, error> Pwrite(std::span<const byte> p, int64 off);
    std::pair<int64, error> Seek(int64 offset, int whence);
    // getdents64(2): 0 at the end of the directory.
    std::pair<isize, error> ReadDirent(std::span<byte> buf);

    error Fstat(struct stat* st);
    error Fstatat(const char* name, struct stat* st, int flags);
    error Fchmod(uint32 mode);
    error Fchown(int uid, int gid);
    error Ftruncate(int64 size);
    error Fsync();
    error Fchdir();

    // Deadlines; ErrNoDeadline for a descriptor that isn't pollable.
    error SetDeadline(time::Time t, int mode);

    // Go's RawRead: calls f(fd) until it returns true, parking until the
    // descriptor is readable after each false. For a descriptor that isn't
    // pollable, a false is an error.
    error RawRead(const std::function<bool(int)>& f);

    int Sysfd() const noexcept { return sysfd_; }
    bool pollable() const noexcept { return pd_ && !blocking_.load(std::memory_order_relaxed); }

    // Marks a regular file or block device, whose reads and writes may
    // block on the disk: they go through io_uring when it's in use. Other
    // blocking descriptors (terminals, pipes and sockets not on the poller)
    // are read and written with plain system calls, which honor O_NONBLOCK
    // if the program sets it.
    void set_regular(bool regular) noexcept { regular_ = regular; }

private:
    // Reference counting of operations against Close (Go's fdMutex,
    // without its per-direction queues: rmu_ and wmu_ serialize reads and
    // writes).
    error incref() noexcept;
    // Drops a reference; the last one after Close closes the descriptor
    // and returns close's error.
    error decref() noexcept;
    error destroy() noexcept;
    error poll_error(int pe) const;
    // Runs a call that takes the descriptor, retrying after EINTR.
    template <typename F>
    error fcall(F&& f);
    // Likewise, through io_uring if it supports opcode.
    template <typename Prep, typename Call>
    error ucall(int opcode, Prep&& prep, Call&& call);

    int sysfd_;
    runtime::poll_desc* pd_ = nullptr;
    std::atomic<bool> blocking_{true};
    bool regular_ = false;
    std::atomic<uint64_t> state_{1};  // bit 63: closed; low bits: references
    std::atomic<uint32_t> csema_{0};  // released when the descriptor is closed
    sync::Mutex rmu_;
    sync::Mutex wmu_;
};

}  // namespace gopp::poll
