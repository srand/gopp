// udp-echo: echo every UDP datagram back to its sender.
// Try: printf 'hello\n' | nc -u -w1 localhost 8082
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>

#include <array>
#include <span>

using namespace gopp;

int main() {
    auto [conn, err] = net::ListenPacket("udp", ":8082");
    if (err) {
        fmt::Println(os::Stderr, "udp-echo: {}", err);
        return 1;
    }
    fmt::Println("UDP echo listening on {}", conn->LocalAddr());

    std::array<byte, 65535> packet{};
    for (;;) {
        auto [n, peer, read_err] = conn->ReadFrom(packet);
        if (read_err) {
            fmt::Println(os::Stderr, "udp-echo: {}", read_err);
            return 1;
        }
        auto payload = std::span<const byte>(packet.data(), static_cast<std::size_t>(n));
        if (auto write_err = conn->WriteTo(payload, peer).second) {
            fmt::Println(os::Stderr, "udp-echo: {}", write_err);
            return 1;
        }
    }
}
