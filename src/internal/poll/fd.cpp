#include "fd.hpp"

#include "../../runtime/runtime.hpp"

#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/syscall.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>

namespace gopp::poll {

const error ErrFileClosing = errors::New("use of closed file");

namespace {

constexpr uint64_t closed_bit = uint64_t(1) << 63;
// Reads and writes of streams are capped like Go's maxRW.
constexpr std::size_t max_rw = std::size_t(1) << 30;

error errno_error(int e) { return syscall::errno_error(e); }

// A system call that may block: on a blocking descriptor, its thread hands
// its processor off meanwhile.
template <typename F>
auto sys(bool blocking, F&& f) {
    if (blocking) {
        runtime::blocking_call b;
        return f();
    }
    return f();
}

}  // namespace

FD::~FD() {
    // Normally closed through Close; this handles a File that wasn't.
    if (!(state_.load(std::memory_order_relaxed) & closed_bit) && sysfd_ >= 0) {
        if (pd_) {
            runtime::poll_unblock(pd_);
            runtime::poll_close(pd_);
        }
        ::close(sysfd_);
    }
}

error FD::Init(bool pollable) {
    if (!pollable) return nullptr;
    int err = 0;
    pd_ = runtime::poll_open(sysfd_, &err);
    if (!pd_) return errno_error(err);
    blocking_.store(false, std::memory_order_relaxed);
    return nullptr;
}

error FD::incref() noexcept {
    uint64_t s = state_.load(std::memory_order_relaxed);
    for (;;) {
        if (s & closed_bit) return ErrFileClosing;
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acquire)) return nullptr;
    }
}

error FD::decref() noexcept {
    uint64_t s = state_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (s == closed_bit) return destroy();
    return nullptr;
}

error FD::destroy() noexcept {
    if (pd_) runtime::poll_close(pd_);
    pd_ = nullptr;
    // close(2) isn't retried after EINTR: on Linux the descriptor is closed
    // anyway.
    int r = ::close(sysfd_);
    int e = errno;
    sysfd_ = -1;
    runtime::semrelease(&csema_);
    return r < 0 ? errno_error(e) : nullptr;
}

error FD::Close() {
    uint64_t s = state_.load(std::memory_order_relaxed);
    for (;;) {
        if (s & closed_bit) return ErrFileClosing;
        // The open reference becomes Close's own until decref below.
        if (state_.compare_exchange_weak(s, s | closed_bit, std::memory_order_acq_rel)) break;
    }
    bool was_blocking = blocking_.load(std::memory_order_relaxed);
    // Wake fibers parked on the descriptor; they fail with ErrFileClosing.
    if (pd_) runtime::poll_unblock(pd_);
    error err = decref();
    // Wait for the operations still using a pollable descriptor, so that
    // it's closed when Close returns. A blocking one can't be interrupted:
    // it's closed when its last operation returns.
    if (!was_blocking) runtime::semacquire(&csema_);
    return err;
}

error FD::SetBlocking() {
    if (error err = incref()) return err;
    blocking_.store(true, std::memory_order_relaxed);
    int fl = ::fcntl(sysfd_, F_GETFL);
    int e = 0;
    if (fl < 0 || ::fcntl(sysfd_, F_SETFL, fl & ~O_NONBLOCK) < 0) e = errno;
    decref();
    return e ? errno_error(e) : nullptr;
}

error FD::poll_error(int pe) const {
    switch (pe) {
    case runtime::poll_err_closing: return ErrFileClosing;
    case runtime::poll_err_timeout: return os::ErrDeadlineExceeded;
    default: return os::ErrNoDeadline;
    }
}

std::pair<isize, error> FD::Read(std::span<byte> p) {
    std::lock_guard l(rmu_);
    if (error err = incref()) return {0, err};
    if (p.empty()) {
        decref();
        return {0, nullptr};
    }
    bool blocking = !pollable();
    if (!blocking) {
        if (int pe = runtime::poll_reset(pd_, 'r')) {
            decref();
            return {0, poll_error(pe)};
        }
    }
    if (p.size() > max_rw) p = p.first(max_rw);
    for (;;) {
        ssize_t n = sys(blocking, [&] { return ::read(sysfd_, p.data(), p.size()); });
        if (n >= 0) {
            decref();
            if (n == 0) return {0, io::EOF_};
            return {static_cast<isize>(n), nullptr};
        }
        int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN && !blocking) {
            if (int pe = runtime::poll_wait(pd_, 'r')) {
                decref();
                return {0, poll_error(pe)};
            }
            continue;
        }
        decref();
        return {0, errno_error(e)};
    }
}

std::pair<isize, error> FD::Pread(std::span<byte> p, int64 off) {
    if (error err = incref()) return {0, err};
    if (p.size() > max_rw) p = p.first(max_rw);
    bool blocking = !pollable();
    for (;;) {
        ssize_t n = sys(blocking, [&] { return ::pread(sysfd_, p.data(), p.size(), off); });
        if (n < 0 && errno == EINTR) continue;
        int e = errno;
        decref();
        if (n < 0) return {0, errno_error(e)};
        if (n == 0 && !p.empty()) return {0, io::EOF_};
        return {static_cast<isize>(n), nullptr};
    }
}

std::pair<isize, error> FD::Write(std::span<const byte> p) {
    std::lock_guard l(wmu_);
    if (error err = incref()) return {0, err};
    bool blocking = !pollable();
    if (!blocking) {
        if (int pe = runtime::poll_reset(pd_, 'w')) {
            decref();
            return {0, poll_error(pe)};
        }
    }
    isize nn = 0;
    for (;;) {
        std::size_t max = std::min(p.size(), max_rw);
        ssize_t n = sys(blocking, [&] { return ::write(sysfd_, p.data(), max); });
        if (n > 0) {
            nn += n;
            p = p.subspan(static_cast<std::size_t>(n));
        }
        if (p.empty()) {
            decref();
            return {nn, nullptr};
        }
        int e = errno;
        if (n < 0 && e == EINTR) continue;
        if (n < 0 && e == EAGAIN && !blocking) {
            if (int pe = runtime::poll_wait(pd_, 'w')) {
                decref();
                return {nn, poll_error(pe)};
            }
            continue;
        }
        if (n > 0) continue;
        decref();
        return {nn, n == 0 ? io::ErrUnexpectedEOF : errno_error(e)};
    }
}

std::pair<isize, error> FD::Pwrite(std::span<const byte> p, int64 off) {
    if (error err = incref()) return {0, err};
    bool blocking = !pollable();
    isize nn = 0;
    while (!p.empty()) {
        std::size_t max = std::min(p.size(), max_rw);
        ssize_t n = sys(blocking, [&] { return ::pwrite(sysfd_, p.data(), max, off + nn); });
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int e = errno;
            decref();
            return {nn, n == 0 ? io::ErrUnexpectedEOF : errno_error(e)};
        }
        nn += n;
        p = p.subspan(static_cast<std::size_t>(n));
    }
    decref();
    return {nn, nullptr};
}

std::pair<int64, error> FD::Seek(int64 offset, int whence) {
    if (error err = incref()) return {0, err};
    off_t r = ::lseek(sysfd_, offset, whence);
    int e = errno;
    decref();
    if (r < 0) return {0, errno_error(e)};
    return {static_cast<int64>(r), nullptr};
}

std::pair<isize, error> FD::ReadDirent(std::span<byte> buf) {
    if (error err = incref()) return {0, err};
    for (;;) {
        ssize_t n = sys(true, [&] { return ::getdents64(sysfd_, buf.data(), buf.size()); });
        if (n < 0 && errno == EINTR) continue;
        int e = errno;
        decref();
        if (n < 0) return {0, errno_error(e)};
        return {static_cast<isize>(n), nullptr};
    }
}

template <typename F>
error FD::fcall(F&& f) {
    if (error err = incref()) return err;
    int r;
    do {
        r = sys(true, f);
    } while (r < 0 && errno == EINTR);
    int e = errno;
    decref();
    return r < 0 ? errno_error(e) : nullptr;
}

error FD::Fstat(struct stat* st) {
    return fcall([&] { return ::fstat(sysfd_, st); });
}

error FD::Fstatat(const char* name, struct stat* st, int flags) {
    return fcall([&] { return ::fstatat(sysfd_, name, st, flags); });
}

error FD::Fchmod(uint32 mode) {
    return fcall([&] { return ::fchmod(sysfd_, mode); });
}

error FD::Fchown(int uid, int gid) {
    return fcall([&] { return ::fchown(sysfd_, static_cast<uid_t>(uid), static_cast<gid_t>(gid)); });
}

error FD::Ftruncate(int64 size) {
    return fcall([&] { return ::ftruncate(sysfd_, size); });
}

error FD::Fsync() {
    return fcall([&] { return ::fsync(sysfd_); });
}

error FD::Fchdir() {
    return fcall([&] { return ::fchdir(sysfd_); });
}

error FD::SetDeadline(time::Time t, int mode) {
    if (error err = incref()) return err;
    if (!pollable()) {
        decref();
        return os::ErrNoDeadline;
    }
    int64 d = 0;
    if (!t.IsZero()) {
        int64 dur = time::Until(t).ns;
        d = dur <= 0 ? -1 : runtime::nanotime() + dur;
    }
    runtime::poll_set_deadline(pd_, d, mode);
    decref();
    return nullptr;
}

}  // namespace gopp::poll
