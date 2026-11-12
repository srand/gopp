// Listening, dialing and connections, after Go's net/net.go, dial.go,
// sock_posix.go, ipsock_posix.go, tcpsock*.go and udpsock*.go.
#include "fd.hpp"
#include "ipsock.hpp"

#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

namespace gopp::net {

const error ErrClosed = errors::New("use of closed network connection");
const error ErrWriteToConnected = errors::New("use of WriteTo with pre-connected connection");

namespace {

const error errMissingAddress = errors::New("missing address");

}  // namespace

string OpError::Error() const {
    string s = Op;
    if (!Net.empty()) s = s + " " + Net;
    if (Source) s = s + " " + Source->String();
    if (Addr) {
        s = s + (Source ? "->" : " ");
        s = s + Addr->String();
    }
    return s + ": " + Err.Error();
}

namespace {

gopp::error op_error(std::string_view op, const string& network, ptr<Addr> source, ptr<Addr> addr, gopp::error err) {
    if (!err || err == io::EOF_) return err;
    auto e = make_ptr<OpError>();
    e->Op = string(op);
    e->Net = network;
    e->Source = std::move(source);
    e->Addr = std::move(addr);
    e->Err = std::move(err);
    return e;
}

int listen_backlog() {
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

bool is_tcp(std::string_view n) { return n == "tcp" || n == "tcp4" || n == "tcp6"; }
bool is_udp(std::string_view n) { return n == "udp" || n == "udp4" || n == "udp6"; }

// The sockaddr of a TCPAddr or UDPAddr for family; len 0 for nullptr.
error addr_sockaddr(const Addr* a, int family, sockaddr_storage* sa, socklen_t* len) {
    *len = 0;
    if (!a) return nullptr;
    ip_port ip = ip_port_of(a);
    return ip_to_sockaddr(family, ip.ip, ip.port, ip.zone.view(), sa, len);
}

// Go's internetSocket and socket: a socket of the favorite family for the
// addresses, bound to laddr and listening (stream) for listen, or
// connected to raddr when it's given.
std::pair<ptr<netfd>, error> internet_socket(std::string_view net, const Addr* laddr, const Addr* raddr, int sotype,
                                             bool listen, int64 deadline) {
    bool ipv6only;
    int family = favorite_family(net, laddr, raddr, listen, &ipv6only);
    int s = ::socket(family, sotype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return {nullptr, os::NewSyscallError("socket", errno_error(errno))};
    int one = 1, v6only = ipv6only;
    if (family == AF_INET6) ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    if (sotype == SOCK_DGRAM) ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    auto fd = make_ptr<netfd>(s, family, sotype, string(net));

    sockaddr_storage lsa, rsa;
    socklen_t llen, rlen;
    if (error err = addr_sockaddr(laddr, family, &lsa, &llen)) return {nullptr, err};
    if (error err = addr_sockaddr(raddr, family, &rsa, &rlen)) return {nullptr, err};
    if (listen && sotype == SOCK_STREAM) ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // A nil laddr listens on the wildcard address, with a port picked.
    if (listen && llen == 0) ip_to_sockaddr(family, {}, 0, {}, &lsa, &llen);
    if (llen != 0 && ::bind(s, reinterpret_cast<sockaddr*>(&lsa), llen) < 0) {
        return {nullptr, os::NewSyscallError("bind", errno_error(errno))};
    }
    if (listen && sotype == SOCK_STREAM && ::listen(s, listen_backlog()) < 0) {
        return {nullptr, os::NewSyscallError("listen", errno_error(errno))};
    }
    if (error err = fd->init()) return {nullptr, err};
    if (rlen != 0) {
        if (error err = fd->connect(&rsa, rlen, deadline)) {
            if (errors::As<Errno>(err)) err = os::NewSyscallError("connect", err);
            return {nullptr, err};
        }
        fd->set_addrs(&rsa);
    } else {
        fd->set_addrs();
    }
    return {fd, nullptr};
}

class TCPListener final : public Listener {
public:
    explicit TCPListener(ptr<netfd> fd) : fd_(std::move(fd)) {}
    ~TCPListener() override { fd_->Close(); }

    std::pair<ptr<Conn>, error> Accept() override {
        auto [nfd, err] = fd_->accept();
        if (err) return {nullptr, op_error("accept", fd_->network(), nullptr, fd_->laddr, err)};
        nfd->setsockopt_int(IPPROTO_TCP, TCP_NODELAY, 1);
        return {make_ptr<TCPConn>(nfd), nullptr};
    }

    error Close() override {
        if (error err = fd_->Close()) return op_error("close", fd_->network(), nullptr, fd_->laddr, err);
        return nullptr;
    }

    ptr<net::Addr> Addr() const override { return fd_->laddr; }

private:
    ptr<netfd> fd_;
};

std::pair<ptr<UDPConn>, error> listen_udp(std::string_view net, const Addr* laddr) {
    auto [fd, err] = internet_socket(net, laddr, nullptr, SOCK_DGRAM, true, 0);
    if (err) return {nullptr, err};
    return {make_ptr<UDPConn>(fd), nullptr};
}

std::pair<ptr<Conn>, error> dial_one(std::string_view net, const Addr* raddr, int64 deadline) {
    if (is_udp(net)) {
        auto [fd, err] = internet_socket(net, nullptr, raddr, SOCK_DGRAM, false, deadline);
        if (err) return {nullptr, err};
        return {make_ptr<UDPConn>(fd), nullptr};
    }
    auto [fd, err] = internet_socket(net, nullptr, raddr, SOCK_STREAM, false, deadline);
    if (err) return {nullptr, err};
    fd->setsockopt_int(IPPROTO_TCP, TCP_NODELAY, 1);
    return {make_ptr<TCPConn>(fd), nullptr};
}

}  // namespace

std::pair<ptr<Listener>, error> Listen(std::string_view network, std::string_view address) {
    string net(network);
    auto [addrs, rerr] = internet_addr_list(network, address);
    if (rerr) return {nullptr, op_error("listen", net, nullptr, nullptr, rerr)};
    ptr<Addr> la = first_ipv4(addrs);
    if (!is_tcp(network)) {
        return {nullptr, op_error("listen", net, nullptr, la,
                                  make_ptr<AddrError>(string::from_static("unexpected address type"), string(address)))};
    }
    auto [fd, err] = internet_socket(network, la.get(), nullptr, SOCK_STREAM, true, 0);
    if (err) return {nullptr, op_error("listen", net, nullptr, la, err)};
    return {make_ptr<TCPListener>(fd), nullptr};
}

std::pair<ptr<PacketConn>, error> ListenPacket(std::string_view network, std::string_view address) {
    string net(network);
    auto [addrs, rerr] = internet_addr_list(network, address);
    if (rerr) return {nullptr, op_error("listen", net, nullptr, nullptr, rerr)};
    ptr<Addr> la = first_ipv4(addrs);
    if (!is_udp(network)) {
        return {nullptr, op_error("listen", net, nullptr, la,
                                  make_ptr<AddrError>(string::from_static("unexpected address type"), string(address)))};
    }
    auto [c, err] = listen_udp(network, la.get());
    if (err) return {nullptr, op_error("listen", net, nullptr, la, err)};
    return {c, nullptr};
}

std::pair<ptr<UDPConn>, error> ListenUDP(std::string_view network, const ptr<UDPAddr>& laddr) {
    string net(network);
    if (!is_udp(network)) return {nullptr, op_error("listen", net, nullptr, laddr, make_ptr<UnknownNetworkError>(net))};
    auto [c, err] = listen_udp(network, laddr.get());
    if (err) return {nullptr, op_error("listen", net, nullptr, laddr, err)};
    return {c, nullptr};
}

std::pair<ptr<UDPConn>, error> DialUDP(std::string_view network, const ptr<UDPAddr>& laddr, const ptr<UDPAddr>& raddr) {
    string net(network);
    if (!is_udp(network)) return {nullptr, op_error("dial", net, laddr, raddr, make_ptr<UnknownNetworkError>(net))};
    if (!raddr) return {nullptr, op_error("dial", net, laddr, nullptr, errMissingAddress)};
    auto [fd, err] = internet_socket(network, laddr.get(), raddr.get(), SOCK_DGRAM, false, 0);
    if (err) return {nullptr, op_error("dial", net, laddr, raddr, err)};
    return {make_ptr<UDPConn>(fd), nullptr};
}

std::pair<ptr<Conn>, error> DialTimeout(std::string_view network, std::string_view address, time::Duration timeout) {
    string net(network);
    auto [addrs, rerr] = internet_addr_list(network, address);
    if (rerr) return {nullptr, op_error("dial", net, nullptr, nullptr, rerr)};
    int64 deadline = timeout.ns > 0 ? runtime::nanotime() + timeout.ns : 0;
    // Go's dialSerial: each address in turn; the first error is reported.
    error first;
    for (auto& a : addrs) {
        auto [c, err] = dial_one(network, a.get(), deadline);
        if (!err) return {c, nullptr};
        if (!first) first = op_error("dial", net, nullptr, a, err);
    }
    return {nullptr, first};
}

std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address) {
    return DialTimeout(network, address, time::Duration(0));
}

// ---------------------------------------------------------------------------
// Connections

conn_base::conn_base(ptr<netfd> fd) : fd_(std::move(fd)) {}

conn_base::~conn_base() { fd_->Close(); }

std::pair<isize, error> conn_base::Read(std::span<byte> p) {
    auto [n, err] = fd_->Read(p);
    return {n, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
}

std::pair<isize, error> conn_base::Write(std::span<const byte> p) {
    auto [n, err] = fd_->Write(p);
    return {n, op_error("write", fd_->network(), fd_->laddr, fd_->raddr, err)};
}

error conn_base::Close() { return op_error("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->Close()); }

ptr<net::Addr> conn_base::LocalAddr() const { return fd_->laddr; }
ptr<net::Addr> conn_base::RemoteAddr() const { return fd_->raddr; }

error conn_base::SetDeadline(time::Time t) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->SetDeadline(t, 'r' + 'w'));
}
error conn_base::SetReadDeadline(time::Time t) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->SetDeadline(t, 'r'));
}
error conn_base::SetWriteDeadline(time::Time t) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->SetDeadline(t, 'w'));
}

error conn_base::SetReadBuffer(int bytes) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->setsockopt_int(SOL_SOCKET, SO_RCVBUF, bytes));
}
error conn_base::SetWriteBuffer(int bytes) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->setsockopt_int(SOL_SOCKET, SO_SNDBUF, bytes));
}

error TCPConn::CloseRead() {
    return op_error("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_RD));
}
error TCPConn::CloseWrite() {
    return op_error("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_WR));
}

error TCPConn::SetNoDelay(bool on) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->setsockopt_int(IPPROTO_TCP, TCP_NODELAY, on));
}

error TCPConn::SetKeepAlive(bool on) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->setsockopt_int(SOL_SOCKET, SO_KEEPALIVE, on));
}

// ---------------------------------------------------------------------------
// UDPConn

std::tuple<isize, ptr<UDPAddr>, error> UDPConn::ReadFromUDP(std::span<byte> b) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, {}, 0, &from);
    if (err) return {n, nullptr, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    ptr<Addr> a = sockaddr_to_addr(from, SOCK_DGRAM);
    return {n, ptr<UDPAddr>(dynamic_cast<UDPAddr*>(a.get())), nullptr};
}

std::tuple<isize, ptr<net::Addr>, error> UDPConn::ReadFrom(std::span<byte> b) {
    auto [n, addr, err] = ReadFromUDP(b);
    return {n, addr, err};
}

std::tuple<isize, isize, int, ptr<UDPAddr>, error> UDPConn::ReadMsgUDP(std::span<byte> b, std::span<byte> oob) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, oob, 0, &from);
    if (err) return {n, oobn, flags, nullptr, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    ptr<Addr> a = sockaddr_to_addr(from, SOCK_DGRAM);
    return {n, oobn, flags, ptr<UDPAddr>(dynamic_cast<UDPAddr*>(a.get())), nullptr};
}

std::pair<isize, error> UDPConn::WriteToUDP(std::span<const byte> b, const ptr<UDPAddr>& addr) {
    auto fail = [&](error err) {
        return std::pair<isize, error>{0, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->raddr) return fail(ErrWriteToConnected);
    if (!addr) return fail(errMissingAddress);
    sockaddr_storage sa;
    socklen_t len;
    if (error err = addr_sockaddr(addr.get(), fd_->family(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, {}, &sa, len);
    if (err) return {n, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, nullptr};
}

std::pair<isize, error> UDPConn::WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) {
    auto* a = dynamic_cast<UDPAddr*>(addr.get());
    if (!a) return {0, op_error("write", fd_->network(), fd_->laddr, addr, errno_error(EINVAL))};
    return WriteToUDP(b, ptr<UDPAddr>(a));
}

std::tuple<isize, isize, error> UDPConn::WriteMsgUDP(std::span<const byte> b, std::span<const byte> oob,
                                                     const ptr<UDPAddr>& addr) {
    auto fail = [&](error err) {
        return std::tuple<isize, isize, error>{0, 0, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->raddr && addr) return fail(ErrWriteToConnected);
    if (!fd_->raddr && !addr) return fail(errMissingAddress);
    sockaddr_storage sa;
    socklen_t len = 0;
    if (error err = addr_sockaddr(addr.get(), fd_->family(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, oob, addr ? &sa : nullptr, len);
    if (err) return {n, oobn, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, oobn, nullptr};
}

}  // namespace gopp::net
