// Address resolution for the IP networks, after Go's net/ipsock.go.
#pragma once

#include <gopp/context.hpp>
#include <gopp/net.hpp>

#include <string_view>
#include <vector>

namespace gopp::net {

using addr_list = std::vector<ptr<Addr>>;


// The IP, port and zone of a TCPAddr or UDPAddr.
struct ip_port {
    IP ip;
    int port = 0;
    string zone;
    bool ok = false;
};
ip_port ip_port_of(const Addr* a);

}  // namespace gopp::net
