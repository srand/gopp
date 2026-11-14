// tcp-echo: a concurrent TCP echo server. Try: nc localhost 8081
// Each accepted connection is handled by its own fiber, and io::Copy from
// the connection to itself moves the bytes with splice(2).
//
//     $ tcp-echo [-addr :8081]
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>

using namespace gopp;

int main(int argc, char** argv) {
    string addr = ":8081";
    flag::StringVar(&addr, "addr", addr, "the address to listen on");
    flag::Parse(argc, argv);

    auto [listener, err] = net::Listen("tcp", addr);
    if (err) {
        fmt::Println(os::Stderr, "tcp-echo: {}", err);
        return 1;
    }
    fmt::Println("TCP echo listening on {}", listener->Addr());

    for (;;) {
        auto [conn, acceptErr] = listener->Accept();
        if (acceptErr) {
            fmt::Println(os::Stderr, "tcp-echo: {}", acceptErr);
            return 1;
        }
        go([conn] {
            auto [n, copyErr] = io::Copy(*conn, *conn);
            fmt::Println("{}: echoed {} bytes{}", conn->RemoteAddr(), n, copyErr ? fmt::Sprint(" ({})", copyErr) : string());
            conn->Close();
        });
    }
}
