// Address resolution for the IP networks, after Go's net/ipsock.go.
#pragma once

#include <gopp/net.hpp>

#include <string_view>
#include <vector>

namespace gopp::net {

using addr_list = std::vector<ptr<Addr>>;

// Go's internetAddrList: the addresses of "host:port" for net ("tcp*",
// "udp*"), as TCPAddrs or UDPAddrs; a missing host gives one with a nil
// IP. Looks names up as a blocking call.
std::pair<addr_list, error> internet_addr_list(std::string_view net, std::string_view addr);

// Go's addrList.first(isIPv4): the first IPv4 address, else the first.
ptr<Addr> first_ipv4(const addr_list& addrs);

// The IP, port and zone of a TCPAddr or UDPAddr.
struct ip_port {
    IP ip;
    int port = 0;
    string zone;
    bool ok = false;
};
ip_port ip_port_of(const Addr* a);

// Go's favoriteAddrFamily: the socket family for network's last character
// and the addresses (ip nil or an IPv4 address: AF_INET). Listening on a
// wildcard uses a dual-stack IPv6 socket where the kernel has one.
// ipv6only is set for "tcp6" and "udp6".
int favorite_family(std::string_view network, const Addr* laddr, const Addr* raddr, bool listen, bool* ipv6only);

}  // namespace gopp::net
