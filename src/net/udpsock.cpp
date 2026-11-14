// UDP, after Go's net/udpsock.go and udpsock_posix.go.
#include "dial.hpp"
#include "ipsock.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace gopp::net {

namespace {

bool isUdp(std::string_view n) { return n == "udp" || n == "udp4" || n == "udp6"; }

// The AddrPort of a sockaddr, without making a UDPAddr (Go's
// readFromAddrPort path).
netip::AddrPort sockaddrAddrport(const sockaddr_storage& sa) {
    if (sa.ss_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(&sa);
        std::array<byte, 4> a;
        std::memcpy(a.data(), &in->sin_addr, 4);
        return netip::AddrPortFrom(netip::AddrFrom4(a), ntohs(in->sin_port));
    }
    if (sa.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(&sa);
        std::array<byte, 16> a;
        std::memcpy(a.data(), &in6->sin6_addr, 16);
        netip::Addr ip = netip::AddrFrom16(a);
        if (in6->sin6_scope_id != 0) {
            // The zone's name, as UDPAddr has it.
            ptr<Addr> ua = sockaddrToAddr(sa, SOCK_DGRAM);
            ip = ip.WithZone(static_cast<UDPAddr*>(ua.get())->Zone.view());
        }
        return netip::AddrPortFrom(ip, ntohs(in6->sin6_port));
    }
    return {};
}

// Go's addrPortToSockaddrInet4 and Inet6, for the socket's family.
error addrportSockaddr(netip::AddrPort ap, int family, sockaddr_storage* sa, socklen_t* len) {
    netip::Addr addr = ap.Addr();
    if (family == AF_INET) {
        // A v4-mapped IPv6 address is fine for an IPv4 socket.
        if (!addr.Is4() && !addr.Is4In6()) {
            return make_ptr<AddrError>(string::fromStatic("non-IPv4 address"), addr.String());
        }
    }
    return ip_to_sockaddr(family, IP(addr.AsSlice()), ap.Port(), addr.Zone().view(), sa, len);
}

}  // namespace

netip::AddrPort UDPAddr::AddrPort() const {
    auto [na, ok] = netip::AddrFromSlice(IP);
    na = na.WithZone(Zone.view());
    return netip::AddrPortFrom(na, uint16(Port));
}

ptr<UDPAddr> UDPAddrFromAddrPort(netip::AddrPort addr) {
    return make_ptr<UDPAddr>(net::IP(addr.Addr().AsSlice()), int(addr.Port()), addr.Addr().Zone());
}

std::tuple<isize, ptr<UDPAddr>, error> UDPConn::ReadFromUDP(std::span<byte> b) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, {}, 0, &from);
    if (err) return {n, nullptr, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    ptr<Addr> a = sockaddrToAddr(from, SOCK_DGRAM);
    return {n, ptr<UDPAddr>(dynamic_cast<UDPAddr*>(a.get())), nullptr};
}

std::tuple<isize, ptr<net::Addr>, error> UDPConn::ReadFrom(std::span<byte> b) {
    auto [n, addr, err] = ReadFromUDP(b);
    return {n, addr, err};
}

std::tuple<isize, netip::AddrPort, error> UDPConn::ReadFromUDPAddrPort(std::span<byte> b) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, {}, 0, &from);
    if (err) return {n, {}, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    return {n, sockaddrAddrport(from), nullptr};
}

std::tuple<isize, isize, int, ptr<UDPAddr>, error> UDPConn::ReadMsgUDP(std::span<byte> b, std::span<byte> oob) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, oob, 0, &from);
    if (err) return {n, oobn, flags, nullptr, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    ptr<Addr> a = sockaddrToAddr(from, SOCK_DGRAM);
    return {n, oobn, flags, ptr<UDPAddr>(dynamic_cast<UDPAddr*>(a.get())), nullptr};
}

std::tuple<isize, isize, int, netip::AddrPort, error> UDPConn::ReadMsgUDPAddrPort(std::span<byte> b,
                                                                                std::span<byte> oob) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, oob, 0, &from);
    if (err) return {n, oobn, flags, {}, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    return {n, oobn, flags, sockaddrAddrport(from), nullptr};
}

std::pair<isize, error> UDPConn::WriteToUDP(std::span<const byte> b, const ptr<UDPAddr>& addr) {
    auto fail = [&](error err) {
        return std::pair<isize, error>{0, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->raddr) return fail(ErrWriteToConnected);
    if (!addr) return fail(errMissingAddress);
    sockaddr_storage sa;
    socklen_t len;
    if (error err = addrSockaddr(addr.get(), fd_->family(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, {}, &sa, len);
    if (err) return {n, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, nullptr};
}

std::pair<isize, error> UDPConn::WriteToUDPAddrPort(std::span<const byte> b, netip::AddrPort addr) {
    auto fail = [&](error err) {
        return std::pair<isize, error>{
            0, op_error("write", fd_->network(), fd_->laddr, UDPAddrFromAddrPort(addr), err)};
    };
    if (fd_->raddr) return fail(ErrWriteToConnected);
    if (!addr.IsValid()) return fail(errMissingAddress);
    sockaddr_storage sa;
    socklen_t len;
    if (error err = addrportSockaddr(addr, fd_->family(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, {}, &sa, len);
    if (err) return fail(err);
    return {n, nullptr};
}

std::pair<isize, error> UDPConn::WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) {
    auto* a = dynamic_cast<UDPAddr*>(addr.get());
    if (!a) return {0, op_error("write", fd_->network(), fd_->laddr, addr, errnoError(EINVAL))};
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
    if (error err = addrSockaddr(addr.get(), fd_->family(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, oob, addr ? &sa : nullptr, len);
    if (err) return {n, oobn, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, oobn, nullptr};
}

std::tuple<isize, isize, error> UDPConn::WriteMsgUDPAddrPort(std::span<const byte> b, std::span<const byte> oob,
                                                             netip::AddrPort addr) {
    auto fail = [&](error err) {
        return std::tuple<isize, isize, error>{
            0, 0, op_error("write", fd_->network(), fd_->laddr, UDPAddrFromAddrPort(addr), err)};
    };
    if (fd_->raddr && addr.IsValid()) return fail(ErrWriteToConnected);
    if (!fd_->raddr && !addr.IsValid()) return fail(errMissingAddress);
    sockaddr_storage sa;
    socklen_t len = 0;
    if (addr.IsValid()) {
        if (error err = addrportSockaddr(addr, fd_->family(), &sa, &len)) return fail(err);
    }
    auto [n, oobn, err] = fd_->writeMsg(b, oob, addr.IsValid() ? &sa : nullptr, len);
    if (err) return fail(err);
    return {n, oobn, nullptr};
}

// ---------------------------------------------------------------------------
// Dialing and listening

std::pair<ptr<UDPConn>, error> sysDialer::dialUDP(const ptr<context::Context>& ctx, const UDPAddr* laddr,
                                                  const UDPAddr* raddr) {
    auto [fd, err] = internetSocket(ctx, network.view(), laddr, raddr, SOCK_DGRAM, 0, "dial", ctrlCtxFn());
    if (err) return {nullptr, err};
    return {make_ptr<UDPConn>(fd), nullptr};
}

std::pair<ptr<UDPConn>, error> sysListener::listenUDP(const ptr<context::Context>& ctx, const UDPAddr* laddr) {
    auto [fd, err] = internetSocket(ctx, network.view(), laddr, nullptr, SOCK_DGRAM, 0, "listen", ctrlCtxFn());
    if (err) return {nullptr, err};
    return {make_ptr<UDPConn>(fd), nullptr};
}

std::pair<ptr<UDPConn>, error> ListenUDP(std::string_view network, const ptr<UDPAddr>& laddr0) {
    string net(network);
    if (!isUdp(network)) return {nullptr, op_error("listen", net, nullptr, laddr0, make_ptr<UnknownNetworkError>(net))};
    ptr<UDPAddr> laddr = laddr0 ? laddr0 : make_ptr<UDPAddr>();
    sysListener sl{.lc = {}, .network = net, .address = laddr->String()};
    auto [c, err] = sl.listenUDP(context::Background(), laddr.get());
    if (err) return {nullptr, op_error("listen", net, nullptr, laddr0, err)};
    return {c, nullptr};
}

std::pair<ptr<UDPConn>, error> DialUDP(std::string_view network, const ptr<UDPAddr>& laddr, const ptr<UDPAddr>& raddr) {
    string net(network);
    if (!isUdp(network)) return {nullptr, op_error("dial", net, laddr, raddr, make_ptr<UnknownNetworkError>(net))};
    if (!raddr) return {nullptr, op_error("dial", net, laddr, nullptr, errMissingAddress)};
    sysDialer sd{.d = {}, .network = net, .address = raddr->String()};
    auto [c, err] = sd.dialUDP(context::Background(), laddr.get(), raddr.get());
    if (err) return {nullptr, op_error("dial", net, laddr, raddr, err)};
    return {c, nullptr};
}

}  // namespace gopp::net
