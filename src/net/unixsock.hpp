// Unix domain sockets, for net.cpp's Listen, Dial and File functions.
#pragma once

#include "fd.hpp"

namespace gopp::net {

// Go's unixSocket: a socket for net ("unix", "unixgram", "unixpacket"),
// bound to laddr if it's given, then listening (listen) or connected to
// raddr.
std::pair<ptr<netfd>, error> unix_socket(std::string_view net, const UnixAddr* laddr, const UnixAddr* raddr,
                                         bool listen);

// Go's listenUnix: a listener that removes its socket file when closed.
std::pair<ptr<UnixListener>, error> listen_unix(std::string_view net, const UnixAddr* laddr);

}  // namespace gopp::net
