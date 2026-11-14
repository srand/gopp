// Address resolution for the IP networks, after Go's net/ipsock.go.
#pragma once

#include <gopp/context.hpp>
#include <gopp/net.hpp>

#include <string_view>
#include <vector>

namespace gopp::net {

using addrList = std::vector<ptr<Addr>>;


// The IP, port and zone of a TCPAddr or UDPAddr.
struct ipPort {
    IP ip;
    int port = 0;
    string zone;
    bool ok = false;
};
ipPort ipPortOf(const Addr* a);

}  // namespace gopp::net
