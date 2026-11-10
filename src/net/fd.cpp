#include "fd.hpp"

#include "../runtime/runtime.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

namespace gopp::net {

namespace {

constexpr uint64_t closed_bit = uint64_t(1) << 63;

}  // namespace

string sockaddr_string(const void* sa) {
    char host[INET6_ADDRSTRLEN];
    auto* s = static_cast<const sockaddr*>(sa);
    if (s->sa_family == AF_INET) {
        auto* in = static_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        return JoinHostPort(host, std::to_string(ntohs(in->sin_port)));
    }
    if (s->sa_family == AF_INET6) {
        auto* in6 = static_cast<const sockaddr_in6*>(sa);
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            inet_ntop(AF_INET, &in6->sin6_addr.s6_addr[12], host, sizeof(host));
        } else {
            inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        }
        return JoinHostPort(host, std::to_string(ntohs(in6->sin6_port)));
    }
    return {};
}

netfd::~netfd() {
    // Normally closed through close(); this handles an fd that never was.
    if (sysfd_ >= 0) {
        if (pd_) {
            runtime::poll_unblock(pd_);
            runtime::poll_close(pd_);
        }
        ::close(sysfd_);
    }
}

error netfd::init() {
    int err = 0;
    pd_ = runtime::poll_open(sysfd_, &err);
    if (!pd_) return errno_error(err);
    return nullptr;
}

bool netfd::incref() noexcept {
    uint64_t s = state_.load(std::memory_order_relaxed);
    for (;;) {
        if (s & closed_bit) return false;
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acquire)) return true;
    }
}

void netfd::decref() noexcept {
    uint64_t s = state_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (s == closed_bit) {
        // Closed and the last operation finished: release the descriptor.
        if (pd_) runtime::poll_close(pd_);
        pd_ = nullptr;
        ::close(sysfd_);
        sysfd_ = -1;
    }
}

error netfd::Close() {
    uint64_t s = state_.load(std::memory_order_relaxed);
    for (;;) {
        if (s & closed_bit) return ErrClosed;
        if (state_.compare_exchange_weak(s, s | closed_bit, std::memory_order_acq_rel)) break;
    }
    // Wake fibers blocked on the descriptor; they fail with ErrClosed.
    if (pd_) runtime::poll_unblock(pd_);
    decref();  // the reference held while open
    return nullptr;
}

error netfd::op_error(int pollerr) const {
    switch (pollerr) {
    case runtime::poll_err_closing: return ErrClosed;
    case runtime::poll_err_timeout: return ErrDeadlineExceeded;
    default: return errors::New("not pollable");
    }
}

std::pair<isize, error> netfd::Read(std::span<byte> p) {
    std::lock_guard l(rmu_);
    if (!incref()) return {0, ErrClosed};
    if (int pe = runtime::poll_reset(pd_, 'r')) {
        decref();
        return {0, op_error(pe)};
    }
    // Large reads are capped like Go's maxRW.
    if (p.size() > (1u << 30)) p = p.first(1u << 30);
    for (;;) {
        ssize_t n = ::read(sysfd_, p.data(), p.size());
        GOPP_STAT(read);
        if (n >= 0) {
            decref();
            if (n == 0 && !p.empty()) return {0, io::EOF_};
            return {static_cast<isize>(n), nullptr};
        }
        int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN) {
            GOPP_STAT(read_eagain);
            if (int pe = runtime::poll_wait(pd_, 'r')) {
                decref();
                return {0, op_error(pe)};
            }
            continue;
        }
        decref();
        return {0, errno_error(e)};
    }
}

std::pair<isize, error> netfd::Write(std::span<const byte> p) {
    std::lock_guard l(wmu_);
    if (!incref()) return {0, ErrClosed};
    if (int pe = runtime::poll_reset(pd_, 'w')) {
        decref();
        return {0, op_error(pe)};
    }
    isize nn = 0;
    while (!p.empty()) {
        std::size_t max = std::min<std::size_t>(p.size(), 1u << 30);
        ssize_t n = ::send(sysfd_, p.data(), max, MSG_NOSIGNAL);
        GOPP_STAT(write);
        if (n > 0) {
            nn += n;
            p = p.subspan(static_cast<std::size_t>(n));
            continue;
        }
        int e = errno;
        if (n < 0 && e == EINTR) continue;
        if (n < 0 && e == EAGAIN) {
            GOPP_STAT(write_eagain);
            if (int pe = runtime::poll_wait(pd_, 'w')) {
                decref();
                return {nn, op_error(pe)};
            }
            continue;
        }
        decref();
        return {nn, n == 0 ? io::ErrUnexpectedEOF : errno_error(e)};
    }
    decref();
    return {nn, nullptr};
}

std::pair<ptr<netfd>, error> netfd::accept() {
    std::lock_guard l(rmu_);
    if (!incref()) return {nullptr, ErrClosed};
    if (int pe = runtime::poll_reset(pd_, 'r')) {
        decref();
        return {nullptr, op_error(pe)};
    }
    for (;;) {
        sockaddr_storage sa;
        socklen_t salen = sizeof(sa);
        int s = ::accept4(sysfd_, reinterpret_cast<sockaddr*>(&sa), &salen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (s >= 0) {
            decref();
            auto nfd = make_ptr<netfd>(s, sa.ss_family, net_);
            if (error err = nfd->init()) return {nullptr, err};
            sockaddr_storage la;
            socklen_t lalen = sizeof(la);
            getsockname(s, reinterpret_cast<sockaddr*>(&la), &lalen);
            nfd->LocalAddr = sockaddr_string(&la);
            nfd->RemoteAddr = sockaddr_string(&sa);
            return {nfd, nullptr};
        }
        int e = errno;
        if (e == EINTR || e == ECONNABORTED) continue;
        if (e == EAGAIN) {
            if (int pe = runtime::poll_wait(pd_, 'r')) {
                decref();
                return {nullptr, op_error(pe)};
            }
            continue;
        }
        decref();
        return {nullptr, errno_error(e)};
    }
}

error netfd::connect(const void* sa, unsigned salen, int64 deadline) {
    if (!incref()) return ErrClosed;
    struct guard {
        netfd* fd;
        ~guard() { fd->decref(); }
    } g{this};

    for (;;) {
        int r = ::connect(sysfd_, static_cast<const sockaddr*>(sa), salen);
        if (r == 0) return nullptr;
        int e = errno;
        if (e == EINTR) continue;
        if (e == EINPROGRESS || e == EALREADY) break;
        if (e == EISCONN) return nullptr;
        return errno_error(e);
    }
    if (deadline != 0) runtime::poll_set_deadline(pd_, deadline, 'w');
    for (;;) {
        if (int pe = runtime::poll_wait(pd_, 'w')) {
            if (deadline != 0) runtime::poll_set_deadline(pd_, 0, 'w');
            return op_error(pe);
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (getsockopt(sysfd_, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) return errno_error(errno);
        if (soerr == EINPROGRESS || soerr == EALREADY || soerr == EINTR) continue;
        if (deadline != 0) runtime::poll_set_deadline(pd_, 0, 'w');
        if (soerr != 0 && soerr != EISCONN) return errno_error(soerr);
        return nullptr;
    }
}

error netfd::shutdown(int how) {
    if (!incref()) return ErrClosed;
    int r = ::shutdown(sysfd_, how);
    int e = errno;
    decref();
    return r < 0 ? errno_error(e) : nullptr;
}

error netfd::SetDeadline(time::Time t, int mode) {
    if (!incref()) return ErrClosed;
    int64 d = 0;
    if (!t.IsZero()) {
        int64 dur = time::Until(t).ns;
        d = dur <= 0 ? -1 : runtime::nanotime() + dur;
    }
    runtime::poll_set_deadline(pd_, d, mode);
    decref();
    return nullptr;
}

}  // namespace gopp::net
