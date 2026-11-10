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
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>

using namespace gopp;

int main(int argc, char** argv) {
    string addr = ":8080", cert, key;
    bool h2c = false;
    flag::StringVar(&addr, "addr", addr, "address to listen on");
    flag::StringVar(&cert, "cert", "", "TLS certificate file (PEM); serves HTTPS");
    flag::StringVar(&key, "key", "", "TLS key file (PEM)");
    flag::BoolVar(&h2c, "h2c", false, "accept HTTP/2 without TLS (prior knowledge)");
    flag::Parse(argc, argv);

    http::ServeMux mux;

    mux.HandleFunc("GET /{$}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "Hello, World!\n");
    });

    mux.HandleFunc("GET /hello/{name}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "Hello, {}!\n", r.PathValue("name"));
    });

    // Sends the request body back.
    mux.HandleFunc("POST /echo", [](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Content-Type", "application/octet-stream");
        io::Copy(w, *r.Body);
    });

    http::Protocols protocols = http::DefaultProtocols();
    protocols.SetUnencryptedHTTP2(h2c);
    http::Server srv{
        .Addr = addr,
        .Handler = make_ptr<http::ServeMux>(std::move(mux)),
        .Protocols = protocols,
    };
    gopp::error err;
    if (!cert.empty()) {
        fmt::Println("listening on {} (HTTPS)", srv.Addr);
        err = srv.ListenAndServeTLS(cert, key);
    } else {
        fmt::Println("listening on {}{}", srv.Addr, h2c ? " (h2c too)" : "");
        err = srv.ListenAndServe();
    }
    fmt::Println("{}", err);
    return 1;
}
