// Address resolution for the IP networks, after Go's net/ipsock.go.
#pragma once

#include <gopp/context.hpp>
#include <gopp/net.hpp>

#include <string_view>
#include <vector>

namespace gopp::net {

using addr_list = std::vector<ptr<Addr>>;

// Go's internetAddrList: the addresses of "host:port" for net ("tcp*",
// "udp*"), as TCPAddrs or UDPAddrs; a missing host gives one with a nil
// IP. Looks names up as a blocking call.
std::pair<addr_list, error> internet_addr_list(const ptr<context::Context>& ctx, std::string_view net,
                                               std::string_view addr);

// The IP, port and zone of a TCPAddr or UDPAddr.
struct ip_port {
    IP ip;
    int port = 0;
    string zone;
    bool ok = false;
};
ip_port ip_port_of(const Addr* a);

}  // namespace gopp::net
