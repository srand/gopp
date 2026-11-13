// Dialing and listening, after Go's net/dial.go, sock_posix.go and the
// sysDialer and sysListener methods of tcpsock_posix.go, udpsock_posix.go
// and unixsock_posix.go.
#pragma once

#include "fd.hpp"

#include <gopp/context.hpp>
#include <gopp/net.hpp>

#include <string_view>
#include <vector>

namespace gopp::net {

using addrList = std::vector<ptr<Addr>>;

// Go's sysDialer: a Dialer and the network and address being dialed.
struct sysDialer {
    Dialer d;
    string network, address;

    // The Control or ControlContext callback, as ControlContext; empty if
    // there's none.
    ControlContextFunc ctrlCtxFn() const;

    std::pair<ptr<Conn>, error> dialParallel(const ptr<context::Context>& ctx, const addrList& primaries,
                                             const addrList& fallbacks);
    std::pair<ptr<Conn>, error> dialSerial(const ptr<context::Context>& ctx, const addrList& ras);
    std::pair<ptr<Conn>, error> dialSingle(const ptr<context::Context>& ctx, const ptr<Addr>& ra);
    std::pair<ptr<TCPConn>, error> dialTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr, const TCPAddr* raddr,
                                           int proto = 0);
    std::pair<ptr<TCPConn>, error> dialMPTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr,
                                             const TCPAddr* raddr);
    std::pair<ptr<UDPConn>, error> dialUDP(const ptr<context::Context>& ctx, const UDPAddr* laddr,
                                           const UDPAddr* raddr);
    std::pair<ptr<UnixConn>, error> dialUnix(const ptr<context::Context>& ctx, const UnixAddr* laddr,
                                             const UnixAddr* raddr);
};

// Go's sysListener: a ListenConfig and the network and address.
struct sysListener {
    ListenConfig lc;
    string network, address;

    ControlContextFunc ctrlCtxFn() const;

    std::pair<ptr<TCPListener>, error> listenTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr, int proto = 0);
    std::pair<ptr<TCPListener>, error> listenMPTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr);
    std::pair<ptr<UDPConn>, error> listenUDP(const ptr<context::Context>& ctx, const UDPAddr* laddr);
    std::pair<ptr<UnixListener>, error> listenUnix(const ptr<context::Context>& ctx, const UnixAddr* laddr);
    std::pair<ptr<UnixConn>, error> listenUnixgram(const ptr<context::Context>& ctx, const UnixAddr* laddr);
};

// Go's socket: a socket of family, type and protocol; with laddr and no
// raddr it listens (streams) or is bound (datagrams), otherwise it's bound
// to laddr if given and connected to raddr if given.
std::pair<ptr<netfd>, error> socket(const ptr<context::Context>& ctx, std::string_view net, int family, int sotype,
                                    int proto, bool ipv6only, const Addr* laddr, const Addr* raddr,
                                    const ControlContextFunc& ctrlCtxFn);

// Go's internetSocket: socket with the favorite family of the addresses;
// mode is "dial" or "listen".
std::pair<ptr<netfd>, error> internetSocket(const ptr<context::Context>& ctx, std::string_view net, const Addr* laddr,
                                            const Addr* raddr, int sotype, int proto, std::string_view mode,
                                            const ControlContextFunc& ctrlCtxFn);

// Go's unixSocket.
std::pair<ptr<netfd>, error> unixSocket(const ptr<context::Context>& ctx, std::string_view net, const UnixAddr* laddr,
                                        const UnixAddr* raddr, std::string_view mode,
                                        const ControlContextFunc& ctrlCtxFn);

// Go's newRawConn and newRawListener (whose Read and Write fail).
ptr<syscall::RawConn> newRawConn(ptr<netfd> fd, bool listener = false);

// Go's newTCPConn: TCP_NODELAY, and keep-alives as configured.
ptr<TCPConn> newTCPConn(ptr<netfd> fd, time::Duration keepAliveIdle, KeepAliveConfig keepAliveCfg);

// Go's supportsMultipathTCP and isUsingMultipathTCP.
bool supportsMultipathTCP();
bool isUsingMultipathTCP(netfd& fd);

// Go's parseNetwork: the address family part of network ("tcp", "ip4")
// and the protocol of "ip:proto".
std::tuple<string, int, error> parseNetwork(const ptr<context::Context>& ctx, std::string_view network,
                                            bool needsProto);

// Go's Resolver.resolveAddrList: the addresses of addr for an op ("dial"
// or "listen") on network; for a dial, those matching hint's family.
std::pair<addrList, error> resolveAddrList(const ptr<context::Context>& ctx, std::string_view op,
                                           std::string_view network, std::string_view addr, const Addr* hint);

// Go's partialDeadline: a share of the time left for one of
// addrsRemaining addresses, at least 2s if there's that much.
std::pair<time::Time, error> partialDeadline(time::Time now, time::Time deadline, int addrsRemaining);

// Go's isIPv4 and addrList.partition, first and forResolve.
bool isIPv4(const Addr* addr);
std::pair<addrList, addrList> partition(const addrList& addrs, bool (*strategy)(const Addr*));
ptr<Addr> first(const addrList& addrs, bool (*strategy)(const Addr*));
ptr<Addr> forResolve(const addrList& addrs, std::string_view network, std::string_view addr);

}  // namespace gopp::net
