// udp-echo: echo every UDP datagram back to its sender.
// Try: printf 'hello\n' | nc -u -w1 localhost 8082
//
//     $ udp-echo [-addr :8082]
//
// The senders are netip::AddrPorts, small values: ReadFromUDPAddrPort and
// WriteToUDPAddrPort don't allocate per packet.
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>

#include <array>
#include <span>

using namespace gopp;

int main(int argc, char** argv) {
    string addr = ":8082";
    flag::StringVar(&addr, "addr", addr, "the address to listen on");
    flag::Parse(argc, argv);

    auto [laddr, resolve_err] = net::ResolveUDPAddr("udp", addr);
    if (resolve_err) {
        fmt::Println(os::Stderr, "udp-echo: {}", resolve_err);
        return 1;
    }
    auto [conn, err] = net::ListenUDP("udp", laddr);
    if (err) {
        fmt::Println(os::Stderr, "udp-echo: {}", err);
        return 1;
    }
    fmt::Println("UDP echo listening on {}", conn->LocalAddr());

    std::array<byte, 65535> packet{};
    for (;;) {
        auto [n, peer, read_err] = conn->ReadFromUDPAddrPort(packet);
        if (read_err) {
            fmt::Println(os::Stderr, "udp-echo: {}", read_err);
            return 1;
        }
        auto payload = std::span<const byte>(packet.data(), static_cast<std::size_t>(n));
        if (auto write_err = conn->WriteToUDPAddrPort(payload, peer).second) {
            fmt::Println(os::Stderr, "udp-echo: {}: {}", peer.String(), write_err);
        }
    }
}
