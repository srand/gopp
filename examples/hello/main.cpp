// Hello world over HTTP: a ServeMux with two routes.
//
//     $ hello [addr]          # default :8080
//     $ curl localhost:8080/
//     Hello, World!
//     $ curl localhost:8080/hello/gopher
//     Hello, gopher!
#include <gopp/fmt.hpp>
#include <gopp/net/http.hpp>

using namespace gopp;

int main(int argc, char** argv) {
    http::ServeMux mux;

    mux.HandleFunc("GET /{$}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "Hello, World!\n");
    });

    mux.HandleFunc("GET /hello/{name}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "Hello, {}!\n", r.PathValue("name"));
    });

    http::Server srv{
        .Addr = argc > 1 ? argv[1] : ":8080",
        .Handler = make_ptr<http::ServeMux>(std::move(mux)),
    };
    fmt::Println("listening on {}", srv.Addr);
    if (auto err = srv.ListenAndServe()) {
        fmt::Println("{}", err);
        return 1;
    }
}
