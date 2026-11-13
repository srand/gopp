#include "fd.hpp"

#include "../runtime/runtime.hpp"

#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>

#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace gopp::net {

namespace {

constexpr uint64_t closed_bit = uint64_t(1) << 63;
// Reads and writes of streams are capped like Go's maxRW.
constexpr std::size_t max_rw = std::size_t(1) << 30;

// Go's zoneCache: interface names and indexes.
string zone_name(uint32_t index) {
    if (index == 0) return {};
    char name[IF_NAMESIZE];
    if (if_indextoname(index, name)) return string(name);
    return string(std::to_string(index));
}

uint32_t zone_index(std::string_view zone) {
    if (zone.empty()) return 0;
    std::string z(zone);
    if (unsigned idx = if_nametoindex(z.c_str())) return idx;
    uint32_t n = 0;
    for (char c : zone) {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + uint32_t(c - '0');
    }
    return n;
}

}  // namespace

string unix_net(int sotype) {
    switch (sotype) {
    case SOCK_DGRAM: return string::from_static("unixgram");
    case SOCK_SEQPACKET: return string::from_static("unixpacket");
    default: return string::from_static("unix");
    }
}

error unix_sockaddr(std::string_view name, sockaddr_storage* sa, socklen_t* len) {
    std::memset(sa, 0, sizeof(*sa));
    auto* un = reinterpret_cast<sockaddr_un*>(sa);
    std::size_t n = name.size();
    if (n > sizeof(un->sun_path)) return errno_error(EINVAL);
    // A path needs room for its NUL; an abstract name doesn't.
    if (n == sizeof(un->sun_path) && name[0] != '@' && name[0] != '\0') return errno_error(EINVAL);
    un->sun_family = AF_UNIX;
    std::memcpy(un->sun_path, name.data(), n);
    socklen_t sl = sizeof(sa_family_t);
    if (n > 0) sl += static_cast<socklen_t>(n) + 1;
    if (un->sun_path[0] == '@' || (un->sun_path[0] == 0 && sl > 3)) {
        un->sun_path[0] = 0;
        sl--;  // an abstract name has no NUL
    }
    *len = sl;
    return nullptr;
}

ptr<Addr> sockaddr_to_addr(const sockaddr_storage& sa, int sotype) {
    if (sa.ss_family == AF_UNIX) {
        // An abstract name shows with '@' for its leading NUL, and ends at
        // the next NUL, as in Go.
        auto* un = reinterpret_cast<const sockaddr_un*>(&sa);
        char path[sizeof(un->sun_path)];
        std::memcpy(path, un->sun_path, sizeof(path));
        if (path[0] == 0) path[0] = '@';
        std::size_t n = 0;
        while (n < sizeof(path) && path[n] != 0) n++;
        return make_ptr<UnixAddr>(string(std::string_view(path, n)), unix_net(sotype));
    }
    IP ip;
    int port = 0;
    string zone;
    if (sa.ss_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(&sa);
        ip = IP(slice<byte>::of(std::span(reinterpret_cast<const byte*>(&in->sin_addr), 4)));
        port = ntohs(in->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(&sa);
        ip = IP(slice<byte>::of(std::span(reinterpret_cast<const byte*>(&in6->sin6_addr), 16)));
        port = ntohs(in6->sin6_port);
        zone = zone_name(in6->sin6_scope_id);
    } else {
        return nullptr;
    }
    if (sotype == SOCK_DGRAM) return make_ptr<UDPAddr>(std::move(ip), port, std::move(zone));
    if (sotype == SOCK_RAW) return make_ptr<IPAddr>(std::move(ip), std::move(zone));
    return make_ptr<TCPAddr>(std::move(ip), port, std::move(zone));
}

error ip_to_sockaddr(int family, const IP& ip0, int port, std::string_view zone, sockaddr_storage* sa,
                     socklen_t* len) {
    std::memset(sa, 0, sizeof(*sa));
    IP ip = ip0;
    if (family == AF_INET) {
        if (ip.empty()) ip = IPv4zero;
        IP ip4 = ip.To4();
        if (ip4.empty()) return make_ptr<AddrError>(string::from_static("non-IPv4 address"), ip.String());
        auto* in = reinterpret_cast<sockaddr_in*>(sa);
        in->sin_family = AF_INET;
        in->sin_port = htons(static_cast<uint16_t>(port));
        std::memcpy(&in->sin_addr, ip4.data(), 4);
        *len = sizeof(sockaddr_in);
        return nullptr;
    }
    if (family == AF_INET6) {
        // An IPv4 wildcard as an IPv6 one, so that a dual-stack socket
        // listens on both.
        if (ip.empty() || ip.Equal(IPv4zero)) ip = IPv6zero;
        IP ip6 = ip.To16();
        if (ip6.empty()) return make_ptr<AddrError>(string::from_static("non-IPv6 address"), ip.String());
        auto* in6 = reinterpret_cast<sockaddr_in6*>(sa);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(static_cast<uint16_t>(port));
        in6->sin6_scope_id = zone_index(zone);
        std::memcpy(&in6->sin6_addr, ip6.data(), 16);
        *len = sizeof(sockaddr_in6);
        return nullptr;
    }
    return make_ptr<InvalidAddrError>(string::from_static("unexpected socket family"));
}

netfd::~netfd() {
    // Normally closed through Close(); this handles an fd that never was.
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

void netfd::set_addrs(const sockaddr_storage* raddr_sa) {
    sockaddr_storage sa{};
    socklen_t len = sizeof(sa);
    if (getsockname(sysfd_, reinterpret_cast<sockaddr*>(&sa), &len) == 0) laddr = sockaddr_to_addr(sa, sotype_);
    if (raddr_sa) {
        raddr = sockaddr_to_addr(*raddr_sa, sotype_);
        return;
    }
    sockaddr_storage pa{};
    len = sizeof(pa);
    if (getpeername(sysfd_, reinterpret_cast<sockaddr*>(&pa), &len) == 0) raddr = sockaddr_to_addr(pa, sotype_);
}

std::pair<ptr<os::File>, error> netfd::dup() {
    if (!incref()) return {nullptr, ErrClosed};
    int ns = ::fcntl(sysfd_, F_DUPFD_CLOEXEC, 0);
    int e = errno;
    decref();
    if (ns < 0) return {nullptr, os::NewSyscallError("dup", errno_error(e))};
    string name = net_ + ":" + (laddr ? laddr->String() : string()) + "->" + (raddr ? raddr->String() : string());
    return {os::NewFile(static_cast<uintptr>(ns), name), nullptr};
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
    if (p.empty()) {
        // As Go: a zero-byte read returns at once; read(2) would return 0,
        // which looks like EOF.
        decref();
        return {0, nullptr};
    }
    if (int pe = runtime::poll_reset(pd_, 'r')) {
        decref();
        return {0, op_error(pe)};
    }
    if (sotype_ == SOCK_STREAM && p.size() > max_rw) p = p.first(max_rw);
    for (;;) {
        ssize_t n = ::read(sysfd_, p.data(), p.size());
        GOPP_STAT(read);
        if (n >= 0) {
            decref();
            // A zero-byte datagram or raw packet isn't EOF.
            if (n == 0 && sotype_ != SOCK_DGRAM && sotype_ != SOCK_RAW) return {0, io::EOF_};
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
    bool stream = sotype_ == SOCK_STREAM;
    isize nn = 0;
    // A packet is sent whole, even an empty one; a stream in pieces.
    do {
        std::size_t max = stream ? std::min(p.size(), max_rw) : p.size();
        ssize_t n = ::send(sysfd_, p.data(), max, MSG_NOSIGNAL);
        GOPP_STAT(write);
        if (n >= 0 && (n > 0 || !stream)) {
            nn += n;
            p = p.subspan(static_cast<std::size_t>(n));
            if (!stream) break;
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
    } while (!p.empty());
    decref();
    return {nn, nullptr};
}

std::tuple<isize, isize, int, error> netfd::readMsg(std::span<byte> p, std::span<byte> oob, int flags,
                                                    sockaddr_storage* from) {
    std::lock_guard l(rmu_);
    std::memset(from, 0, sizeof(*from));
    if (!incref()) return {0, 0, 0, ErrClosed};
    if (int pe = runtime::poll_reset(pd_, 'r')) {
        decref();
        return {0, 0, 0, op_error(pe)};
    }
    // As Go's Recvmsg: with no buffer for data but some for control
    // messages, a stream socket reads one byte, which carries them.
    byte dummy;
    bool use_dummy = p.empty() && !oob.empty() && sotype_ != SOCK_DGRAM && sotype_ != SOCK_RAW;
    for (;;) {
        iovec iov{use_dummy ? &dummy : p.data(), use_dummy ? 1 : p.size()};
        msghdr msg{};
        msg.msg_name = from;
        msg.msg_namelen = sizeof(*from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (!oob.empty()) {
            msg.msg_control = oob.data();
            msg.msg_controllen = oob.size();
        }
        ssize_t n = ::recvmsg(sysfd_, &msg, flags | MSG_CMSG_CLOEXEC);
        if (n >= 0) {
            decref();
            if (msg.msg_namelen == 0) from->ss_family = AF_UNSPEC;
            // Go's eofError: no bytes from a connection-oriented socket.
            error err = n == 0 && sotype_ != SOCK_DGRAM && sotype_ != SOCK_RAW ? io::EOF_ : nullptr;
            return {static_cast<isize>(n), static_cast<isize>(msg.msg_controllen), msg.msg_flags, err};
        }
        int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN) {
            if (int pe = runtime::poll_wait(pd_, 'r')) {
                decref();
                return {0, 0, 0, op_error(pe)};
            }
            continue;
        }
        decref();
        return {0, 0, 0, errno_error(e)};
    }
}

std::tuple<isize, isize, error> netfd::writeMsg(std::span<const byte> p, std::span<const byte> oob, const void* sa,
                                                unsigned salen) {
    std::lock_guard l(wmu_);
    if (!incref()) return {0, 0, ErrClosed};
    if (int pe = runtime::poll_reset(pd_, 'w')) {
        decref();
        return {0, 0, op_error(pe)};
    }
    // As Go's Sendmsg: control messages without data go with one byte on a
    // stream socket, which doesn't count.
    byte dummy = 0;
    bool use_dummy = p.empty() && !oob.empty() && sotype_ != SOCK_DGRAM && sotype_ != SOCK_RAW;
    for (;;) {
        iovec iov{use_dummy ? &dummy : const_cast<byte*>(p.data()), use_dummy ? 1 : p.size()};
        msghdr msg{};
        msg.msg_name = const_cast<void*>(sa);
        msg.msg_namelen = sa ? salen : 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (!oob.empty()) {
            msg.msg_control = const_cast<byte*>(oob.data());
            msg.msg_controllen = oob.size();
        }
        ssize_t n = ::sendmsg(sysfd_, &msg, MSG_NOSIGNAL);
        if (n >= 0) {
            decref();
            if (use_dummy) n = 0;
            return {static_cast<isize>(n), static_cast<isize>(oob.size()), nullptr};
        }
        int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN) {
            if (int pe = runtime::poll_wait(pd_, 'w')) {
                decref();
                return {0, 0, op_error(pe)};
            }
            continue;
        }
        decref();
        return {0, 0, errno_error(e)};
    }
}

std::pair<ptr<netfd>, error> netfd::accept() {
    std::lock_guard l(rmu_);
    if (!incref()) return {nullptr, ErrClosed};
    if (int pe = runtime::poll_reset(pd_, 'r')) {
        decref();
        return {nullptr, op_error(pe)};
    }
    for (;;) {
        sockaddr_storage sa{};
        socklen_t salen = sizeof(sa);
        int s = ::accept4(sysfd_, reinterpret_cast<sockaddr*>(&sa), &salen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (s >= 0) {
            decref();
            auto nfd = make_ptr<netfd>(s, family_, sotype_, net_);
            if (error err = nfd->init()) return {nullptr, err};
            nfd->set_addrs(&sa);
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

error netfd::setsockopt_int(int level, int name, int value) {
    if (!incref()) return ErrClosed;
    int r = ::setsockopt(sysfd_, level, name, &value, sizeof(value));
    int e = errno;
    decref();
    return r < 0 ? os::NewSyscallError("setsockopt", errno_error(e)) : nullptr;
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

error netfd::connect(const ptr<context::Context>& ctx, const void* sa, unsigned salen, sockaddr_storage* peer,
                     bool* havePeer) {
    *havePeer = false;
    if (!incref()) return ErrClosed;
    struct guard {
        netfd* fd;
        ~guard() { fd->decref(); }
    } g{this};

    int e = 0;
    for (;;) {
        e = ::connect(sysfd_, static_cast<const sockaddr*>(sa), salen) == 0 ? 0 : errno;
        if (e != EINTR) break;
    }
    switch (e) {
    case EINPROGRESS:
    case EALREADY:
        break;
    case 0:
    case EISCONN:
        if (ctx->Err()) return mapErr(ctx->Err());
        return nullptr;
    default:
        return os::NewSyscallError("connect", errno_error(e));
    }

    // Done when ctx is: a write deadline in the past wakes the wait (Go's
    // context.AfterFunc). 0: waiting, 1: fired, 2: stopped.
    auto done = ctx->Done();
    auto state = std::make_shared<std::atomic<int>>(0);
    chan<empty> stop;
    if (done) {
        if (auto d = ctx->Deadline()) SetDeadline(*d, 'w');
        stop = make_chan<empty>();
        ptr<netfd> self(this);
        go([done, stop, state, self] {
            select(recv(done, [&](empty) {
                       int want = 0;
                       if (state->compare_exchange_strong(want, 1)) runtime::poll_set_deadline(self->pd_, -1, 'w');
                   }),
                   recv(stop, [](empty) {}));
        });
    }
    auto finish = [&](error ret) -> error {
        if (done) {
            int want = 0;
            bool stopped = state->compare_exchange_strong(want, 2);
            stop.close();
            runtime::poll_set_deadline(pd_, 0, 'w');
            if (!stopped && !ret) ret = mapErr(ctx->Err());
        }
        return ret;
    };

    for (;;) {
        if (int pe = runtime::poll_wait(pd_, 'w')) {
            if (done && ctx->Err()) return finish(mapErr(ctx->Err()));
            return finish(op_error(pe));
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (getsockopt(sysfd_, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) {
            return finish(os::NewSyscallError("getsockopt", errno_error(errno)));
        }
        switch (soerr) {
        case EINPROGRESS:
        case EALREADY:
        case EINTR:
            continue;
        case EISCONN:
            return finish(nullptr);
        case 0: {
            // The connection may have been refused yet look connected;
            // getpeername tells.
            socklen_t plen = sizeof(*peer);
            std::memset(peer, 0, sizeof(*peer));
            if (getpeername(sysfd_, reinterpret_cast<sockaddr*>(peer), &plen) == 0) {
                *havePeer = true;
                return finish(nullptr);
            }
            continue;
        }
        default:
            return finish(os::NewSyscallError("connect", errno_error(soerr)));
        }
    }
}

std::pair<int64, error> netfd::writeBuffers(std::vector<slice<byte>>& v) {
    std::lock_guard l(wmu_);
    if (!incref()) return {0, ErrClosed};
    if (int pe = runtime::poll_reset(pd_, 'w')) {
        decref();
        return {0, op_error(pe)};
    }
    // Go's maxVec: the iovecs of one writev(2).
    constexpr std::size_t max_vec = 1024;
    int64 total = 0;
    error err;
    std::vector<iovec> iovs;
    std::size_t first = 0;  // the first buffer not yet written in full
    std::size_t off = 0;    // bytes of it written
    for (;;) {
        while (first < v.size() && v[first].size() == off) {
            first++;
            off = 0;
        }
        if (first == v.size()) break;
        iovs.clear();
        for (std::size_t i = first; i < v.size() && iovs.size() < max_vec; i++) {
            std::size_t skip = i == first ? off : 0;
            if (v[i].size() == skip) continue;
            iovs.push_back({v[i].data() + skip, v[i].size() - skip});
        }
        ssize_t n = ::writev(sysfd_, iovs.data(), static_cast<int>(iovs.size()));
        if (n < 0) {
            int e = errno;
            if (e == EINTR) continue;
            if (e == EAGAIN) {
                if (int pe = runtime::poll_wait(pd_, 'w')) {
                    err = op_error(pe);
                    break;
                }
                continue;
            }
            err = os::NewSyscallError("writev", errno_error(e));
            break;
        }
        total += n;
        // Consume n bytes.
        std::size_t left = static_cast<std::size_t>(n);
        while (left > 0) {
            std::size_t room = v[first].size() - off;
            if (left < room) {
                off += left;
                left = 0;
            } else {
                left -= room;
                first++;
                off = 0;
            }
        }
    }
    decref();
    // Drop what was written.
    std::vector<slice<byte>> rest;
    for (std::size_t i = first; i < v.size(); i++) rest.push_back(i == first ? v[i].Sub(off) : v[i]);
    v = std::move(rest);
    return {total, err};
}

error netfd::raw_control(const std::function<void(uintptr)>& f) {
    if (!incref()) return ErrClosed;
    f(static_cast<uintptr>(sysfd_));
    decref();
    return nullptr;
}

error netfd::raw_read(const std::function<bool(uintptr)>& f) {
    std::lock_guard l(rmu_);
    if (!incref()) return ErrClosed;
    if (int pe = runtime::poll_reset(pd_, 'r')) {
        decref();
        return op_error(pe);
    }
    for (;;) {
        if (f(static_cast<uintptr>(sysfd_))) break;
        if (int pe = runtime::poll_wait(pd_, 'r')) {
            decref();
            return op_error(pe);
        }
    }
    decref();
    return nullptr;
}

error netfd::raw_write(const std::function<bool(uintptr)>& f) {
    std::lock_guard l(wmu_);
    if (!incref()) return ErrClosed;
    if (int pe = runtime::poll_reset(pd_, 'w')) {
        decref();
        return op_error(pe);
    }
    for (;;) {
        if (f(static_cast<uintptr>(sysfd_))) break;
        if (int pe = runtime::poll_wait(pd_, 'w')) {
            decref();
            return op_error(pe);
        }
    }
    decref();
    return nullptr;
}

error netfd::prepare(int mode) {
    if (!incref()) return ErrClosed;
    int pe = runtime::poll_reset(pd_, mode);
    decref();
    return pe ? op_error(pe) : nullptr;
}

error netfd::wait(int mode) {
    if (!incref()) return ErrClosed;
    int pe = runtime::poll_wait(pd_, mode);
    decref();
    return pe ? op_error(pe) : nullptr;
}

error netfd::getsockopt_int(int level, int name, int* value) {
    if (!incref()) return ErrClosed;
    socklen_t len = sizeof(*value);
    int r = ::getsockopt(sysfd_, level, name, value, &len);
    int e = errno;
    decref();
    return r < 0 ? errno_error(e) : nullptr;
}

error netfd::setsockopt_linger(int sec) {
    if (!incref()) return ErrClosed;
    linger l{};
    if (sec >= 0) {
        l.l_onoff = 1;
        l.l_linger = sec;
    }
    int r = ::setsockopt(sysfd_, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    int e = errno;
    decref();
    return r < 0 ? os::NewSyscallError("setsockopt", errno_error(e)) : nullptr;
}

}  // namespace gopp::net
