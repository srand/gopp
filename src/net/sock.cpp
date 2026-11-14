// Sockets, after Go's net/sock_posix.go, sock_linux.go, ipsock_posix.go,
// sockopt_linux.go, unixsock_posix.go (unixSocket), mptcpsock_linux.go and
// rawconn.go.
#include "dial.hpp"
#include "ipsock.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <mutex>

namespace gopp::net {

namespace {

// Go's listenerBacklog: /proc/sys/net/core/somaxconn, capped at 65535.
int listenerBacklog() {
    static int backlog = [] {
        int n = SOMAXCONN;
        if (FILE* f = std::fopen("/proc/sys/net/core/somaxconn", "r")) {
            int v;
            if (std::fscanf(f, "%d", &v) == 1 && v > 0) n = v > 65535 ? 65535 : v;
            std::fclose(f);
        }
        return n;
    }();
    return backlog;
}

// Go's sysSocket: a non-blocking, close-on-exec socket.
std::pair<int, error> sysSocket(int family, int sotype, int proto) {
    int s = ::socket(family, sotype | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
    if (s < 0) return {-1, os::NewSyscallError("socket", errnoError(errno))};
    return {s, nullptr};
}

// Go's ipStackCapabilities.
struct ipStackCapabilities {
    bool ipv4Enabled = false;
    bool ipv6Enabled = false;
    bool ipv4MappedIPv6Enabled = false;
};

const ipStackCapabilities& ipStack() {
    static ipStackCapabilities p = [] {
        ipStackCapabilities p;
        if (auto [s, err] = sysSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP); !err) {
            ::close(s);
            p.ipv4Enabled = true;
        }
        // IPv6 if ::1 binds on an IPv6-only socket; IPv4-mapped IPv6 if
        // 127.0.0.1 binds on a dual-stack one.
        struct {
            IP ip;
            int v6only;
        } probes[] = {{ParseIP("::1"), 1}, {IPv4(127, 0, 0, 1), 0}};
        for (int i = 0; i < 2; i++) {
            auto [s, err] = sysSocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
            if (err) continue;
            ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &probes[i].v6only, sizeof(int));
            sockaddr_storage sa;
            socklen_t len;
            bool ok = !ip_to_sockaddr(AF_INET6, probes[i].ip, 0, {}, &sa, &len) &&
                      (::bind(s, reinterpret_cast<sockaddr*>(&sa), len) == 0 || errno == EPERM || errno == EACCES);
            ::close(s);
            if (!ok) continue;
            if (i == 0) p.ipv6Enabled = true;
            else p.ipv4MappedIPv6Enabled = true;
        }
        return p;
    }();
    return p;
}

// The sockaddr interface of Go's IP addresses: family and isWildcard.
int addrFamily(const Addr* a) {
    ip_port ip = ipPortOf(a);
    if (!a || ip.ip.size() <= IPv4len) return AF_INET;
    if (!ip.ip.To4().empty()) return AF_INET;
    return AF_INET6;
}

bool isWildcard(const Addr* a) {
    if (!a) return true;
    if (auto* u = dynamic_cast<const UnixAddr*>(a)) return u->Name.empty();
    ip_port ip = ipPortOf(a);
    return ip.ip.empty() || ip.ip.IsUnspecified();
}

// Go's favoriteAddrFamily.
std::pair<int, bool> favoriteAddrFamily(std::string_view network, const Addr* laddr, const Addr* raddr,
                                        std::string_view mode) {
    switch (network.back()) {
    case '4': return {AF_INET, false};
    case '6': return {AF_INET6, true};
    }
    if (mode == "listen" && isWildcard(laddr)) {
        if (ipStack().ipv4MappedIPv6Enabled || !ipStack().ipv4Enabled) return {AF_INET6, false};
        if (!laddr) return {AF_INET, false};
        return {addrFamily(laddr), false};
    }
    if ((!laddr || addrFamily(laddr) == AF_INET) && (!raddr || addrFamily(raddr) == AF_INET)) return {AF_INET, false};
    return {AF_INET6, false};
}

// Go's setDefaultSockopts.
error setDefaultSockopts(int s, int family, int sotype, bool ipv6only) {
    if (family == AF_INET6 && sotype != SOCK_RAW) {
        // Allow both IP versions even if the OS default is otherwise.
        int v = ipv6only;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof(v));
    }
    if ((sotype == SOCK_DGRAM || sotype == SOCK_RAW) && family != AF_UNIX) {
        // Allow broadcast.
        int one = 1;
        if (::setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
            return os::NewSyscallError("setsockopt", errnoError(errno));
        }
    }
    return nullptr;
}

error setsockoptOne(int s, int level, int name) {
    int one = 1;
    if (::setsockopt(s, level, name, &one, sizeof(one)) < 0) return os::NewSyscallError("setsockopt", errnoError(errno));
    return nullptr;
}

// Go's netFD.ctrlNetwork: the network with its family, for Control.
string ctrlNetwork(const netfd& fd) {
    std::string_view net = fd.network().view();
    if (net == "unix" || net == "unixgram" || net == "unixpacket") return fd.network();
    if (net.back() == '4' || net.back() == '6') return fd.network();
    return fd.network() + (fd.family() == AF_INET ? "4" : "6");
}

error bindAddr(netfd& fd, const sockaddr_storage& sa, socklen_t len) {
    if (::bind(fd.sysfd(), reinterpret_cast<const sockaddr*>(&sa), len) < 0) {
        return os::NewSyscallError("bind", errnoError(errno));
    }
    return nullptr;
}

// Go's netFD.dial.
error dial(netfd& fd, const ptr<context::Context>& ctx, const Addr* laddr, const Addr* raddr,
           const ControlContextFunc& ctrlCtxFn) {
    if (ctrlCtxFn) {
        auto c = newRawConn(ptr<netfd>(&fd));
        string ctrlAddr = raddr ? raddr->String() : laddr ? laddr->String() : string();
        if (error err = ctrlCtxFn(ctx, ctrlNetwork(fd).view(), ctrlAddr.view(), *c)) return err;
    }
    sockaddr_storage lsa, rsa;
    socklen_t llen = 0, rlen = 0;
    if (laddr) {
        if (error err = addrSockaddr(laddr, fd.family(), &lsa, &llen)) return err;
        if (llen != 0) {
            if (error err = bindAddr(fd, lsa, llen)) return err;
        }
    }
    if (error err = fd.init()) return err;
    if (raddr) {
        if (error err = addrSockaddr(raddr, fd.family(), &rsa, &rlen)) return err;
        sockaddr_storage crsa;
        bool connected;
        if (error err = fd.connect(ctx, &rsa, rlen, &crsa, &connected)) return err;
        // The address actually connected to, when the kernel says.
        fd.setAddrs(connected ? &crsa : nullptr);
        if (!fd.raddr) fd.raddr = ptr<Addr>(const_cast<Addr*>(raddr));
        return nullptr;
    }
    fd.setAddrs();
    return nullptr;
}

// Go's netFD.listenStream.
error listenStream(netfd& fd, const ptr<context::Context>& ctx, const Addr* laddr, int backlog,
                   const ControlContextFunc& ctrlCtxFn) {
    if (error err = setsockoptOne(fd.sysfd(), SOL_SOCKET, SO_REUSEADDR)) return err;
    sockaddr_storage lsa;
    socklen_t llen;
    if (error err = addrSockaddr(laddr, fd.family(), &lsa, &llen)) return err;
    if (ctrlCtxFn) {
        auto c = newRawConn(ptr<netfd>(&fd));
        if (error err = ctrlCtxFn(ctx, ctrlNetwork(fd).view(), laddr->String().view(), *c)) return err;
    }
    if (error err = bindAddr(fd, lsa, llen)) return err;
    if (::listen(fd.sysfd(), backlog) < 0) return os::NewSyscallError("listen", errnoError(errno));
    if (error err = fd.init()) return err;
    fd.setAddrs();
    fd.raddr = nullptr;
    return nullptr;
}

// Go's netFD.listenDatagram.
error listenDatagram(netfd& fd, const ptr<context::Context>& ctx, const Addr* laddr,
                     const ControlContextFunc& ctrlCtxFn) {
    ptr<UDPAddr> wild;
    if (auto* u = dynamic_cast<const UDPAddr*>(laddr); u && !u->IP.empty() && u->IP.IsMulticast()) {
        // A multicast group: several sockets may listen on it, each bound
        // to the wildcard address with the group's port.
        if (error err = setsockoptOne(fd.sysfd(), SOL_SOCKET, SO_REUSEADDR)) return err;
        wild = make_ptr<UDPAddr>(fd.family() == AF_INET ? IPv4zero : IPv6unspecified, u->Port, u->Zone);
        laddr = wild.get();
    }
    sockaddr_storage lsa;
    socklen_t llen;
    if (error err = addrSockaddr(laddr, fd.family(), &lsa, &llen)) return err;
    if (ctrlCtxFn) {
        auto c = newRawConn(ptr<netfd>(&fd));
        if (error err = ctrlCtxFn(ctx, ctrlNetwork(fd).view(), laddr->String().view(), *c)) return err;
    }
    if (error err = bindAddr(fd, lsa, llen)) return err;
    if (error err = fd.init()) return err;
    fd.setAddrs();
    fd.raddr = nullptr;
    return nullptr;
}

// Go's rawConn and rawListener.
class rawConn final : public syscall::RawConn {
public:
    rawConn(ptr<netfd> fd, bool listener) : fd_(std::move(fd)), listener_(listener) {}

    error Control(std::function<void(uintptr)> f) override {
        if (error err = fd_->rawControl(f)) return op_error("raw-control", fd_->network(), nullptr, fd_->laddr, err);
        return nullptr;
    }
    error Read(std::function<bool(uintptr)> f) override {
        if (listener_) return errnoError(EINVAL);
        if (error err = fd_->rawRead(f)) return op_error("raw-read", fd_->network(), fd_->laddr, fd_->raddr, err);
        return nullptr;
    }
    error Write(std::function<bool(uintptr)> f) override {
        if (listener_) return errnoError(EINVAL);
        if (error err = fd_->rawWrite(f)) return op_error("raw-write", fd_->network(), fd_->laddr, fd_->raddr, err);
        return nullptr;
    }

private:
    ptr<netfd> fd_;
    bool listener_;
};

// MPTCP (mptcpsock_linux.go).
constexpr int _IPPROTO_MPTCP = 0x106;
constexpr int _SOL_MPTCP = 0x11c;
constexpr int _MPTCP_INFO = 0x1;

bool kernelVersionGE(int major, int minor) {
    utsname u;
    if (::uname(&u) != 0) return false;
    int ma = 0, mi = 0;
    if (std::sscanf(u.release, "%d.%d", &ma, &mi) < 2) return false;
    return ma > major || (ma == major && mi >= minor);
}

struct mptcpState {
    bool available = false;
    bool hasSOLMPTCP = false;  // only valid if available
};

const mptcpState& mptcp() {
    static mptcpState m = [] {
        mptcpState m;
        int family = ipStack().ipv4Enabled ? AF_INET : AF_INET6;
        int s = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, _IPPROTO_MPTCP);
        int e = s < 0 ? errno : 0;
        if (s >= 0) ::close(s);
        // EPROTONOSUPPORT: not supported (>= 5.6); EINVAL: (< 5.6).
        if (e == EPROTONOSUPPORT || e == EINVAL) return m;
        m.available = true;
        m.hasSOLMPTCP = kernelVersionGE(5, 16);
        return m;
    }();
    return m;
}

}  // namespace

ptr<syscall::RawConn> newRawConn(ptr<netfd> fd, bool listener) { return make_ptr<rawConn>(std::move(fd), listener); }

bool supportsMultipathTCP() { return mptcp().available; }

bool isUsingMultipathTCP(netfd& fd) {
    if (!supportsMultipathTCP()) return false;
    if (mptcp().hasSOLMPTCP) {
        // A connection that fell back to TCP has no MPTCP_INFO.
        int v;
        error err = fd.getsockoptInt(_SOL_MPTCP, _MPTCP_INFO, &v);
        auto* e = errors::As<Errno>(err);
        return !(e && (e->Code == EOPNOTSUPP || e->Code == ENOPROTOOPT));
    }
    int proto = 0;
    fd.getsockoptInt(SOL_SOCKET, SO_PROTOCOL, &proto);
    return proto == _IPPROTO_MPTCP;
}

std::pair<ptr<TCPConn>, error> sysDialer::dialMPTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr,
                                                    const TCPAddr* raddr) {
    // Plain TCP if MPTCP isn't there or fails.
    if (supportsMultipathTCP()) {
        auto [c, err] = dialTCP(ctx, laddr, raddr, _IPPROTO_MPTCP);
        if (!err) return {c, nullptr};
    }
    return dialTCP(ctx, laddr, raddr);
}

std::pair<ptr<TCPListener>, error> sysListener::listenMPTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr) {
    if (supportsMultipathTCP()) {
        auto [ln, err] = listenTCP(ctx, laddr, _IPPROTO_MPTCP);
        if (!err) return {ln, nullptr};
    }
    return listenTCP(ctx, laddr);
}

std::pair<ptr<netfd>, error> socket(const ptr<context::Context>& ctx, std::string_view net, int family, int sotype,
                                    int proto, bool ipv6only, const Addr* laddr, const Addr* raddr,
                                    const ControlContextFunc& ctrlCtxFn) {
    auto [s, err] = sysSocket(family, sotype, proto);
    if (err) return {nullptr, err};
    if (error err = setDefaultSockopts(s, family, sotype, ipv6only)) {
        ::close(s);
        return {nullptr, err};
    }
    auto fd = make_ptr<netfd>(s, family, sotype, string(net));

    // A local address and no remote one: a listener, or a datagram socket
    // bound to the address, whose peer (if any) is set by its writes.
    if (laddr && !raddr) {
        if (sotype == SOCK_STREAM || sotype == SOCK_SEQPACKET) {
            if (error err = listenStream(*fd, ctx, laddr, listenerBacklog(), ctrlCtxFn)) {
                fd->Close();
                return {nullptr, err};
            }
            return {fd, nullptr};
        }
        if (sotype == SOCK_DGRAM) {
            if (error err = listenDatagram(*fd, ctx, laddr, ctrlCtxFn)) {
                fd->Close();
                return {nullptr, err};
            }
            return {fd, nullptr};
        }
    }
    if (error err = dial(*fd, ctx, laddr, raddr, ctrlCtxFn)) {
        fd->Close();
        return {nullptr, err};
    }
    return {fd, nullptr};
}

std::pair<ptr<netfd>, error> internetSocket(const ptr<context::Context>& ctx, std::string_view net, const Addr* laddr,
                                            const Addr* raddr, int sotype, int proto, std::string_view mode,
                                            const ControlContextFunc& ctrlCtxFn) {
    auto [family, ipv6only] = favoriteAddrFamily(net, laddr, raddr, mode);
    return socket(ctx, net, family, sotype, proto, ipv6only, laddr, raddr, ctrlCtxFn);
}

std::pair<ptr<netfd>, error> unixSocket(const ptr<context::Context>& ctx, std::string_view net, const UnixAddr* laddr,
                                        const UnixAddr* raddr, std::string_view mode,
                                        const ControlContextFunc& ctrlCtxFn) {
    int sotype;
    if (net == "unix") sotype = SOCK_STREAM;
    else if (net == "unixgram") sotype = SOCK_DGRAM;
    else if (net == "unixpacket") sotype = SOCK_SEQPACKET;
    else return {nullptr, make_ptr<UnknownNetworkError>(string(net))};
    if (mode == "dial") {
        if (laddr && isWildcard(laddr)) laddr = nullptr;
        if (raddr && isWildcard(raddr)) raddr = nullptr;
        if (!raddr && (sotype != SOCK_DGRAM || !laddr)) return {nullptr, errMissingAddress};
    } else if (mode != "listen") {
        return {nullptr, errors::New("unknown mode: " + std::string(mode))};
    }
    return socket(ctx, net, AF_UNIX, sotype, 0, false, laddr, raddr, ctrlCtxFn);
}

}  // namespace gopp::net
