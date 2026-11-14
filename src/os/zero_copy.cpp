// File.ReadFrom, WriteTo and SyscallConn, after Go 1.27.3's os/file.go,
// zero_copy_linux.go, zero_copy_posix.go and rawconn.go, and
// internal/poll's sendfile_unix.go and splice_linux.go.
#include "internal.hpp"

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <limits>

namespace gopp::os {

namespace {

// Go's rawConn of a File.
class rawConn final : public syscall::RawConn {
public:
    rawConn(ptr<File> f, poll::FD& pfd) : f_(std::move(f)), pfd_(pfd) {}

    error Control(std::function<void(uintptr)> fn) override {
        return wrap("control", pfd_.RawControl([&](int fd) { fn(uintptr(fd)); }));
    }
    error Read(std::function<bool(uintptr)> fn) override {
        return wrap("read", pfd_.RawRead([&](int fd) { return fn(uintptr(fd)); }));
    }
    error Write(std::function<bool(uintptr)> fn) override {
        return wrap("write", pfd_.RawWrite([&](int fd) { return fn(uintptr(fd)); }));
    }

private:
    error wrap(std::string_view op, error err) {
        if (!err) return nullptr;
        if (err == poll::ErrFileClosing) err = ErrClosed;
        return detail::path_error(op, f_->Name().view(), err);
    }
    ptr<File> f_;  // keeps the file, and so pfd_, alive
    poll::FD& pfd_;
};

// Go's tryLimitedReader.
std::tuple<io::LimitedReader*, io::Reader*, int64> tryLimitedReader(io::Reader& r) {
    if (auto* lr = dynamic_cast<io::LimitedReader*>(&r)) return {lr, lr->R.get(), lr->N};
    return {nullptr, &r, std::numeric_limits<int64>::max()};
}

// The RawConn of a stream socket (Go's getPollFDAndNetwork and IsStream),
// with whether it's TCP or a Unix stream socket.
std::pair<ptr<syscall::RawConn>, bool> streamSocket(io::Reader* r0, io::Writer* w0) {
    syscall::Conn* sc = r0 ? dynamic_cast<syscall::Conn*>(r0) : dynamic_cast<syscall::Conn*>(w0);
    if (!sc || dynamic_cast<File*>(sc)) return {nullptr, false};
    auto [rc, err] = sc->SyscallConn();
    if (err || !rc) return {nullptr, false};
    int type = 0, domain = 0;
    rc->Control([&](uintptr fd) {
        socklen_t len = sizeof(int);
        if (::getsockopt(int(fd), SOL_SOCKET, SO_TYPE, &type, &len) < 0) type = 0;
        len = sizeof(int);
        if (::getsockopt(int(fd), SOL_SOCKET, SO_DOMAIN, &domain, &len) < 0) domain = 0;
    });
    if (type != SOCK_STREAM) return {nullptr, false};
    return {rc, domain == AF_INET || domain == AF_INET6 || domain == AF_UNIX};
}

error wrapSyscallError(std::string_view name, const error& err) {
    if (errors::As<syscall::Errno>(err) && !errors::As<SyscallError>(err)) return NewSyscallError(string(name), err);
    return err;
}

// io::Copy's loop, without the ReadFrom and WriteTo calls that would lead
// back here (Go's fileWithoutReadFrom and fileWithoutWriteTo).
struct writerOnly final : io::Writer {
    io::Writer& w;
    explicit writerOnly(io::Writer& w_) : w(w_) {}
    std::pair<isize, error> Write(std::span<const byte> p) override { return w.Write(p); }
};

struct readerOnly final : io::Reader {
    io::Reader& r;
    explicit readerOnly(io::Reader& r_) : r(r_) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return r.Read(p); }
};

constexpr int maxSpliceSize = 1 << 20;

}  // namespace

std::pair<ptr<syscall::RawConn>, error> File::SyscallConn() {
    f_->ready();
    return {make_ptr<rawConn>(ptr<File>(this), f_->pfd), nullptr};
}

std::pair<int64, error> File::ReadFrom(io::Reader& r) {
    f_->ready();
    int64 n = 0;
    bool handled = false;
    error e;
    if (!f_->append_mode) {
        // Go's copyFileRange: from another file.
        auto [lr, src, remain] = tryLimitedReader(r);
        if (remain <= 0) return {0, nullptr};
        // A File's WriteTo falls back to its reader without WriteTo (Go's
        // fileWithoutWriteTo): the file is still the source.
        if (auto* ro = dynamic_cast<readerOnly*>(src)) src = &ro->r;
        if (auto* sf = dynamic_cast<File*>(src)) {
            sf->f_->ready();
            std::tie(n, handled, e) = f_->pfd.CopyFileRange(sf->f_->pfd, remain);
            if (lr) lr->N -= n;
            e = wrapSyscallError("copy_file_range", e);
        } else if (auto [rc, inet] = streamSocket(src, nullptr); rc) {
            // Go's spliceToFile: from a stream socket, through a pipe.
            int p[2];
            if (::pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0) {
                ::fcntl(p[0], F_SETPIPE_SZ, maxSpliceSize);
                int inPipe = 0;
                for (;;) {
                    if (remain <= 0) break;
                    int max = int(std::min<int64>(maxSpliceSize, remain));
                    int got = 0, de = 0;
                    error rerr = rc->Read([&](uintptr sfd) {
                        for (;;) {
                            ssize_t k = ::splice(int(sfd), nullptr, p[1], nullptr, std::size_t(max), SPLICE_F_NONBLOCK);
                            if (k >= 0) {
                                got = int(k);
                                return true;
                            }
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN) return false;
                            de = errno;
                            return true;
                        }
                    });
                    if (rerr) {
                        e = rerr;
                        handled = true;
                        break;
                    }
                    if (de) {
                        // EINVAL at once: splice can't do it; copy instead.
                        handled = handled || de != EINVAL;
                        e = handled ? wrapSyscallError("splice", syscall::errnoError(de)) : nullptr;
                        break;
                    }
                    handled = true;
                    if (got == 0) break;  // EOF
                    inPipe += got;
                    // From the pipe into the file.
                    error werr = f_->pfd.RawWrite([&](int dfd) {
                        runtime::blockingCall b;
                        while (inPipe > 0) {
                            ssize_t k = ::splice(p[0], nullptr, dfd, nullptr, std::size_t(inPipe), 0);
                            if (k > 0) {
                                inPipe -= int(k);
                                n += k;
                                remain -= k;
                                continue;
                            }
                            if (k < 0 && errno == EINTR) continue;
                            de = k < 0 ? errno : EIO;
                            break;
                        }
                        return true;
                    });
                    if (werr || de) {
                        e = werr ? werr : wrapSyscallError("splice", syscall::errnoError(de));
                        break;
                    }
                }
                ::close(p[0]);
                ::close(p[1]);
                if (lr) lr->N = lr->N - n;
            }
        }
    }
    if (!handled) {
        writerOnly w(*this);
        return io::Copy(w, r);  // without wrapping
    }
    return {n, wrapErr("write", e)};
}

std::pair<int64, error> File::WriteTo(io::Writer& w) {
    f_->ready();
    // Go's writeTo: sendfile(2) to a TCP or Unix stream socket.
    if (auto [rc, inetOrUnix] = streamSocket(nullptr, &w); rc && inetOrUnix) {
        int64 written = 0;
        int se = 0;
        error werr;
        error cerr = f_->pfd.RawControl([&](int src) {
            werr = rc->Write([&](uintptr dst) {
                for (;;) {
                    ssize_t k = ::sendfile(int(dst), src, nullptr, (std::size_t(1) << 31) - 1);
                    if (k > 0) {
                        written += k;
                        continue;
                    }
                    if (k == 0) return true;  // the end of the file
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN) return false;
                    se = errno;
                    return true;
                }
            });
        });
        if (!cerr) cerr = werr;
        // Go's poll.SendFile: these say sendfile can't do it (a pipe, say).
        bool handled = !((se == ENOSYS || se == EOPNOTSUPP || se == EINVAL) && written == 0);
        if (handled) {
            error e = cerr;
            if (se > 0) e = wrapSyscallError("sendfile", syscall::errnoError(se));
            return {written, wrapErr("read", e)};
        }
    }
    readerOnly r(*this);
    return io::Copy(w, r);  // without wrapping
}

}  // namespace gopp::os
