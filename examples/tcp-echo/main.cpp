// tcp-echo: a concurrent TCP echo server. Try: nc localhost 8081
// Each accepted connection is handled by its own fiber.
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>

using namespace gopp;

int main() {
    auto [listener, err] = net::Listen("tcp", ":8081");
    if (err) {
        fmt::Println(os::Stderr, "tcp-echo: {}", err);
        return 1;
    }
    fmt::Println("TCP echo listening on {}", listener->Addr());

    for (;;) {
        auto [conn, accept_err] = listener->Accept();
        if (accept_err) {
            fmt::Println(os::Stderr, "tcp-echo: {}", accept_err);
            return 1;
        }
        go([conn] {
            io::Copy(*conn, *conn);
            conn->Close();
        });
    }
}
