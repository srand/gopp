#include "fd.hpp"

#include "../../runtime/runtime.hpp"
#include "../../runtime/uring.hpp"

#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/syscall.hpp>

#include <dirent.h>
#include "../../runtime/liburing.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>

namespace gopp::poll {

const error ErrFileClosing = errors::New("use of closed file");

namespace {

constexpr uint64_t closedBit = uint64_t(1) << 63;
// Reads and writes of streams are capped like Go's maxRW.
constexpr std::size_t maxRw = std::size_t(1) << 30;

error errnoError(int e) { return syscall::errnoError(e); }

// A system call that may block: on a blocking descriptor, its thread hands
// its processor off meanwhile.
template <typename F>
auto sys(bool blocking, F&& f) {
    if (blocking) {
        runtime::blockingCall b;
        return f();
    }
    return f();
}

// A read, pread, write or pwrite on a blocking descriptor, through
// io_uring if it can and useUring is set: off is the offset, or -1 for
// the file's. Returns the count or -errno.
long blockingRw(bool useUring, bool write, int fd, void* p, std::size_t n, int64 off) {
    using runtime::uringOrBlocking;
    auto u64off = static_cast<__u64>(off);
    auto len = static_cast<unsigned>(n);
    if (write) {
        return uringOrBlocking(
            useUring ? IORING_OP_WRITE : -1, [&](io_uring_sqe* sqe) { io_uring_prep_write(sqe, fd, p, len, u64off); },
            [&] { return off < 0 ? ::write(fd, p, n) : ::pwrite(fd, p, n, off); });
    }
    return uringOrBlocking(
        useUring ? IORING_OP_READ : -1, [&](io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd, p, len, u64off); },
        [&] { return off < 0 ? ::read(fd, p, n) : ::pread(fd, p, n, off); });
}

}  // namespace

void statFromStatx(const struct statx& x, struct stat* st) noexcept {
    *st = {};
    st->st_dev = makedev(x.stx_dev_major, x.stx_dev_minor);
    st->st_ino = x.stx_ino;
    st->st_mode = x.stx_mode;
    st->st_nlink = x.stx_nlink;
    st->st_uid = x.stx_uid;
    st->st_gid = x.stx_gid;
    st->st_rdev = makedev(x.stx_rdev_major, x.stx_rdev_minor);
    st->st_size = static_cast<off_t>(x.stx_size);
    st->st_blksize = static_cast<blksize_t>(x.stx_blksize);
    st->st_blocks = static_cast<blkcnt_t>(x.stx_blocks);
    st->st_atim = {x.stx_atime.tv_sec, x.stx_atime.tv_nsec};
    st->st_mtim = {x.stx_mtime.tv_sec, x.stx_mtime.tv_nsec};
    st->st_ctim = {x.stx_ctime.tv_sec, x.stx_ctime.tv_nsec};
}

int statat(int dirfd, const char* name, struct stat* st, int flags) noexcept {
    if (!runtime::uringWorth(IORING_OP_STATX)) {
        for (;;) {
            int r, e;
            {
                runtime::blockingCall b;
                r = ::fstatat(dirfd, name, st, flags);
                e = errno;
            }
            if (r == 0) return 0;
            if (e != EINTR) return e;
        }
    }
    struct statx x;
    for (;;) {
        int r = runtime::uring([&](io_uring_sqe* sqe) {
            io_uring_prep_statx(sqe, dirfd, name, flags, STATX_BASIC_STATS, &x);
        });
        if (r == 0) {
            statFromStatx(x, st);
            return 0;
        }
        if (r != -EINTR) return -r;
    }
}

FD::~FD() {
    // Normally closed through Close; this handles a File that wasn't.
    if (!(state_.load(std::memory_order_relaxed) & closedBit) && sysfd_ >= 0) {
        if (pd_) {
            runtime::pollUnblock(pd_);
            runtime::pollClose(pd_);
        }
        ::close(sysfd_);
    }
}

error FD::Init(bool pollable) {
    if (!pollable) return nullptr;
    int err = 0;
    pd_ = runtime::pollOpen(sysfd_, &err);
    if (!pd_) return errnoError(err);
    blocking_.store(false, std::memory_order_relaxed);
    return nullptr;
}

error FD::incref() noexcept {
    uint64_t s = state_.load(std::memory_order_relaxed);
    for (;;) {
        if (s & closedBit) return ErrFileClosing;
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acquire)) return nullptr;
    }
}

error FD::decref() noexcept {
    uint64_t s = state_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (s == closedBit) return destroy();
    return nullptr;
}

error FD::destroy() noexcept {
    if (pd_) runtime::pollClose(pd_);
    pd_ = nullptr;
    // close(2) isn't retried after EINTR: on Linux the descriptor is closed
    // anyway.
    int r = ::close(sysfd_);
    int e = errno;
    sysfd_ = -1;
    runtime::semrelease(&csema_);
    return r < 0 ? errnoError(e) : nullptr;
}

error FD::Close() {
    uint64_t s = state_.load(std::memory_order_relaxed);
    for (;;) {
        if (s & closedBit) return ErrFileClosing;
        // The open reference becomes Close's own until decref below.
        if (state_.compare_exchange_weak(s, s | closedBit, std::memory_order_acq_rel)) break;
    }
    bool wasBlocking = blocking_.load(std::memory_order_relaxed);
    // Wake fibers parked on the descriptor; they fail with ErrFileClosing.
    if (pd_) runtime::pollUnblock(pd_);
    error err = decref();
    // Wait for the operations still using a pollable descriptor, so that
    // it's closed when Close returns. A blocking one can't be interrupted:
    // it's closed when its last operation returns.
    if (!wasBlocking) runtime::semacquire(&csema_);
    return err;
}

error FD::SetBlocking() {
    if (error err = incref()) return err;
    blocking_.store(true, std::memory_order_relaxed);
    int fl = ::fcntl(sysfd_, F_GETFL);
    int e = 0;
    if (fl < 0 || ::fcntl(sysfd_, F_SETFL, fl & ~O_NONBLOCK) < 0) e = errno;
    decref();
    return e ? errnoError(e) : nullptr;
}

error FD::pollError(int pe) const {
    switch (pe) {
    case runtime::pollErrClosing: return ErrFileClosing;
    case runtime::pollErrTimeout: return os::ErrDeadlineExceeded;
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
        if (int pe = runtime::pollReset(pd_, 'r')) {
            decref();
            return {0, pollError(pe)};
        }
    }
    if (p.size() > maxRw) p = p.first(maxRw);
    if (blocking) {
        long n = blockingRw(regular_, false, sysfd_, p.data(), p.size(), -1);
        decref();
        if (n < 0) return {0, errnoError(static_cast<int>(-n))};
        if (n == 0) return {0, io::EOF_};
        return {static_cast<isize>(n), nullptr};
    }
    for (;;) {
        ssize_t n = ::read(sysfd_, p.data(), p.size());
        if (n >= 0) {
            decref();
            if (n == 0) return {0, io::EOF_};
            return {static_cast<isize>(n), nullptr};
        }
        int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN && !blocking) {
            if (int pe = runtime::pollWait(pd_, 'r')) {
                decref();
                return {0, pollError(pe)};
            }
            continue;
        }
        decref();
        return {0, errnoError(e)};
    }
}

std::pair<isize, error> FD::Pread(std::span<byte> p, int64 off) {
    if (error err = incref()) return {0, err};
    if (p.size() > maxRw) p = p.first(maxRw);
    long n;
    if (!pollable()) {
        n = blockingRw(regular_, false, sysfd_, p.data(), p.size(), off);
    } else {
        do {
            n = ::pread(sysfd_, p.data(), p.size(), off);
        } while (n < 0 && errno == EINTR);
        if (n < 0) n = -errno;
    }
    decref();
    if (n < 0) return {0, errnoError(static_cast<int>(-n))};
    if (n == 0 && !p.empty()) return {0, io::EOF_};
    return {static_cast<isize>(n), nullptr};
}

std::pair<isize, error> FD::Write(std::span<const byte> p) {
    std::lock_guard l(wmu_);
    if (error err = incref()) return {0, err};
    bool blocking = !pollable();
    if (!blocking) {
        if (int pe = runtime::pollReset(pd_, 'w')) {
            decref();
            return {0, pollError(pe)};
        }
    }
    isize nn = 0;
    for (;;) {
        std::size_t max = std::min(p.size(), maxRw);
        ssize_t n;
        if (blocking) {
            long r = blockingRw(regular_, true, sysfd_, const_cast<byte*>(p.data()), max, -1);
            n = r < 0 ? -1 : r;
            if (r < 0) errno = static_cast<int>(-r);
        } else {
            n = ::write(sysfd_, p.data(), max);
        }
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
            if (int pe = runtime::pollWait(pd_, 'w')) {
                decref();
                return {nn, pollError(pe)};
            }
            continue;
        }
        if (n > 0) continue;
        decref();
        return {nn, n == 0 ? io::ErrUnexpectedEOF : errnoError(e)};
    }
}

std::pair<isize, error> FD::Pwrite(std::span<const byte> p, int64 off) {
    if (error err = incref()) return {0, err};
    bool blocking = !pollable();
    isize nn = 0;
    while (!p.empty()) {
        std::size_t max = std::min(p.size(), maxRw);
        long n;
        if (blocking) {
            n = blockingRw(regular_, true, sysfd_, const_cast<byte*>(p.data()), max, off + nn);
        } else {
            n = ::pwrite(sysfd_, p.data(), max, off + nn);
            if (n < 0) {
                if (errno == EINTR) continue;
                n = -errno;
            }
        }
        if (n <= 0) {
            decref();
            return {nn, n == 0 ? io::ErrUnexpectedEOF : errnoError(static_cast<int>(-n))};
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
    if (r < 0) return {0, errnoError(e)};
    return {static_cast<int64>(r), nullptr};
}

std::pair<isize, error> FD::ReadDirent(std::span<byte> buf) {
    if (error err = incref()) return {0, err};
    for (;;) {
        ssize_t n = sys(true, [&] { return ::getdents64(sysfd_, buf.data(), buf.size()); });
        if (n < 0 && errno == EINTR) continue;
        int e = errno;
        decref();
        if (n < 0) return {0, errnoError(e)};
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
    return r < 0 ? errnoError(e) : nullptr;
}

error FD::Fstat(struct stat* st) {
    if (error err = incref()) return err;
    int e = statat(sysfd_, "", st, AT_EMPTY_PATH);
    decref();
    return e ? errnoError(e) : nullptr;
}

error FD::Fstatat(const char* name, struct stat* st, int flags) {
    if (error err = incref()) return err;
    int e = statat(sysfd_, name, st, flags);
    decref();
    return e ? errnoError(e) : nullptr;
}

error FD::Fchmod(uint32 mode) {
    return fcall([&] { return ::fchmod(sysfd_, mode); });
}

error FD::Fchown(int uid, int gid) {
    return fcall([&] { return ::fchown(sysfd_, static_cast<uid_t>(uid), static_cast<gid_t>(gid)); });
}

// A call on the descriptor through io_uring if it supports opcode.
template <typename Prep, typename Call>
error FD::ucall(int opcode, Prep&& prep, Call&& call) {
    if (error err = incref()) return err;
    long r = runtime::uringOrBlocking(opcode, prep, call);
    decref();
    return r < 0 ? errnoError(static_cast<int>(-r)) : nullptr;
}

error FD::Ftruncate(int64 size) {
    return ucall(
        IORING_OP_FTRUNCATE, [&](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, sysfd_, size); },
        [&] { return ::ftruncate(sysfd_, size); });
}

error FD::Fsync() {
    return ucall(
        IORING_OP_FSYNC, [&](io_uring_sqe* sqe) { io_uring_prep_fsync(sqe, sysfd_, 0); },
        [&] { return ::fsync(sysfd_); });
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
    runtime::pollSetDeadline(pd_, d, mode);
    decref();
    return nullptr;
}

error FD::RawRead(const std::function<bool(int)>& f) {
    std::lock_guard l(rmu_);
    if (error err = incref()) return err;
    bool polled = pollable();
    if (polled) {
        if (int pe = runtime::pollReset(pd_, 'r')) {
            decref();
            return pollError(pe);
        }
    }
    for (;;) {
        if (f(sysfd_)) {
            decref();
            return nullptr;
        }
        if (!polled) {
            decref();
            return errors::New("waiting for unsupported file type");
        }
        if (int pe = runtime::pollWait(pd_, 'r')) {
            decref();
            return pollError(pe);
        }
    }
}

error FD::RawWrite(const std::function<bool(int)>& f) {
    std::lock_guard l(wmu_);
    if (error err = incref()) return err;
    bool polled = pollable();
    if (polled) {
        if (int pe = runtime::pollReset(pd_, 'w')) {
            decref();
            return pollError(pe);
        }
    }
    for (;;) {
        if (f(sysfd_)) {
            decref();
            return nullptr;
        }
        if (!polled) {
            decref();
            return errors::New("waiting for unsupported file type");
        }
        if (int pe = runtime::pollWait(pd_, 'w')) {
            decref();
            return pollError(pe);
        }
    }
}

error FD::RawControl(const std::function<void(int)>& f) {
    if (error err = incref()) return err;
    f(sysfd_);
    decref();
    return nullptr;
}

std::tuple<int64, bool, error> FD::CopyFileRange(FD& src, int64 remain) {
    // The most copy_file_range copies at once (Go's maxCopyFileRangeRound).
    constexpr int64 maxCopyFileRangeRound = 0x7ffff000;
    int64 written = 0;
    while (remain > 0) {
        std::size_t max = std::size_t(std::min(remain, maxCopyFileRangeRound));
        int64 n = 0;
        int e = 0;
        {
            std::lock_guard wl(wmu_);
            std::lock_guard rl(src.rmu_);
            if (error err = incref()) return {written, written > 0, err};
            if (error err = src.incref()) {
                decref();
                return {written, written > 0, err};
            }
            runtime::blockingCall b;
            for (;;) {
                ssize_t r = ::copy_file_range(src.sysfd_, nullptr, sysfd_, nullptr, max, 0);
                if (r < 0 && errno == EINTR) continue;
                n = r < 0 ? 0 : r;
                e = r < 0 ? errno : 0;
                break;
            }
            src.decref();
            decref();
        }
        if (n > 0) {
            remain -= n;
            written += n;
        }
        // Go's handleCopyFileRangeErr: these say copy_file_range can't do
        // it, and the caller copies instead; so does copying nothing at
        // all (a file in /proc, say).
        switch (e) {
        case ENOSYS:
        case EXDEV:
        case EINVAL:
        case EIO:
        case EOPNOTSUPP:
        case EPERM:
            if (written == 0) return {0, false, nullptr};
            return {written, true, errnoError(e)};
        case 0:
            if (n == 0) return {written, written > 0, nullptr};
            break;
        default:
            return {written, true, errnoError(e)};
        }
    }
    return {written, true, nullptr};
}

}  // namespace gopp::poll
