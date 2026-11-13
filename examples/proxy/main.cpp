// proxy: a TCP proxy. Each connection is dialed through to -to, and two
// fibers copy the bytes both ways. Between TCP sockets io::Copy moves them
// with splice(2), so they never pass through user space.
//
//     $ proxy -listen :8443 -to go.dev:443
//     $ curl --connect-to go.dev:443:localhost:8443 https://go.dev/
//
// When one side finishes sending, the other side's writing is shut down
// (CloseWrite), so a half-closed connection stays half-open, as with a
// direct connection. Ctrl-C stops accepting and waits for the connections
// open to finish.
#include <gopp/context.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/os/signal.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <atomic>

using namespace gopp;
namespace signal = os::signal;

// Copies from src to dst until src ends, then shuts down writing to dst,
// which sends the peer an EOF while the other direction goes on.
int64 relay(net::Conn& dst, net::Conn& src) {
    auto [n, err] = io::Copy(dst, src);
    if (auto* tcp = dynamic_cast<net::TCPConn*>(&dst)) tcp->CloseWrite();
    return n;
}

int main(int argc, char** argv) {
    string listen = ":9000", to;
    time::Duration timeout = 5 * time::Second;
    flag::StringVar(&listen, "listen", listen, "the address to listen on");
    flag::StringVar(&to, "to", "", "the address to proxy to (host:port)");
    flag::DurationVar(&timeout, "timeout", timeout, "the time limit for connecting to -to");
    flag::Parse(argc, argv);
    if (to.empty() || !flag::Args().empty()) {
        fmt::Println(os::Stderr, "usage: proxy [-listen addr] -to host:port");
        return 2;
    }

    auto [ctx, stop] = signal::NotifyContext(context::Background(), os::Interrupt, syscall::SIGTERM_);
    net::ListenConfig lc;
    auto [ln, err] = lc.Listen(ctx, "tcp", listen);
    if (err) {
        fmt::Println(os::Stderr, "proxy: {}", err);
        return 1;
    }
    fmt::Println("proxying {} to {}", ln->Addr(), to);

    // Ctrl-C closes the listener, which ends the accept loop.
    go([ctx, ln] {
        ctx->Done().recv();
        ln->Close();
    });

    // The Dialer resolves -to with Go's resolver, and races IPv6 and IPv4
    // addresses (Happy Eyeballs) within the timeout.
    net::Dialer dialer{.Timeout = timeout};
    std::atomic<int> active{0};
    sync::WaitGroup conns;
    for (;;) {
        auto [client, aerr] = ln->Accept();
        if (aerr) {
            if (ctx->Err()) break;
            fmt::Println(os::Stderr, "proxy: {}", aerr);
            continue;
        }
        conns.Go([&, client] {
            auto [server, derr] = dialer.DialContext(ctx, "tcp", to);
            if (derr) {
                fmt::Println(os::Stderr, "proxy: {}", derr);
                client->Close();
                return;
            }
            fmt::Println("{} -> {} open ({} active)", client->RemoteAddr(), server->RemoteAddr(), ++active);
            time::Time start = time::Now();

            sync::WaitGroup both;
            int64 sent = 0;
            both.Go([&] { sent = relay(*server, *client); });
            int64 received = relay(*client, *server);
            both.Wait();
            client->Close();
            server->Close();
            fmt::Println("{} closed: {} bytes sent, {} received in {} ({} active)", client->RemoteAddr(), sent, received,
                         time::Since(start), --active);
        });
    }
    stop();
    if (active > 0) fmt::Println("waiting for {} connections", active.load());
    conns.Wait();
    return 0;
}
