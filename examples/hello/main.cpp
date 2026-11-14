// Hello world over HTTP: a ServeMux with a few routes.
//
//     $ hello [-addr :8080] [-cert cert.pem -key key.pem] [-h2c]
//     $ curl localhost:8080/
//     Hello, World!
//     $ curl localhost:8080/hello/gopher
//     Hello, gopher!
//     $ curl --data-binary @file localhost:8080/echo
//
// With -cert and -key it serves HTTPS, and HTTP/2 to clients that ask for
// it (curl -k --http2 https://localhost:8080/). With -h2c it also accepts
// HTTP/2 without TLS from clients that know (curl --http2-prior-knowledge).
// Ctrl-C (or SIGTERM) shuts it down gracefully, letting requests in flight
// finish. benchmarks/hello-go is the same server in Go, for the benchmark.
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/os.hpp>
#include <gopp/os/signal.hpp>
#include <gopp/runtime.hpp>
#include <gopp/syscall.hpp>

using namespace gopp;
namespace signal = os::signal;

int main(int argc, char** argv) {
    string addr = ":8080", cert, key;
    bool h2c = false;
    flag::StringVar(&addr, "addr", addr, "address to listen on");
    flag::StringVar(&cert, "cert", "", "TLS certificate file (PEM); serves HTTPS");
    flag::StringVar(&key, "key", "", "TLS key file (PEM)");
    flag::BoolVar(&h2c, "h2c", false, "accept HTTP/2 without TLS (prior knowledge)");
    flag::Parse(argc, argv);

    auto mux = http::NewServeMux();

    mux->HandleFunc("GET /{$}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "Hello, World!\n");
    });

    mux->HandleFunc("GET /hello/{name}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "Hello, {}!\n", r.PathValue("name"));
    });

    // Sends the request body back.
    mux->HandleFunc("POST /echo", [](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Content-Type", string::fromStatic("application/octet-stream"));
        io::Copy(w, *r.Body);
    });

    http::Protocols protocols = http::DefaultProtocols();
    protocols.SetUnencryptedHTTP2(h2c);
    http::Server srv{.Addr = addr, .Handler = mux, .Protocols = protocols};

    auto served = make_chan<error>(1);
    go([&] { served.send(cert.empty() ? srv.ListenAndServe() : srv.ListenAndServeTLS(cert, key)); });
    fmt::Println("listening on {}{}", srv.Addr, !cert.empty() ? " (HTTPS)" : h2c ? " (h2c too)" : "");

    auto [ctx, stop] = signal::NotifyContext(context::Background(), os::Interrupt, syscall::SIGTERM_);
    error err;
    select(recv(served, [&](error e) { err = e; }), recv(ctx->Done(), [] {}));
    stop();
    if (err) {
        fmt::Println(os::Stderr, "hello: {}", err);
        return 1;
    }
    fmt::Println("shutting down");
    auto [sctx, cancel] = context::WithTimeout(context::Background(), 5 * time::Second);
    srv.Shutdown(sctx);
    cancel();
    served.recv();
    return 0;
}
