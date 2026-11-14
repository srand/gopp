// httpfileserver: serves a directory over HTTP with http::FileServerFS.
//
//     $ httpfileserver -dir ./public -addr :8080
//     $ curl localhost:8080/                       # index.html, or a listing
//     $ curl -r 0-99 localhost:8080/big.iso        # a range: 206 Partial Content
//     $ curl -z big.iso localhost:8080/big.iso     # If-Modified-Since: 304
//
// The directory is opened as an os::Root, so no request can reach outside
// it: "..", absolute names and symbolic links that point out are refused.
// (http::Dir would follow such links.) Every request is logged with its
// status, and Ctrl-C shuts the server down gracefully: it stops accepting
// connections and waits for the requests in flight.
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/os.hpp>
#include <gopp/os/signal.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

using namespace gopp;
namespace signal = os::signal;

// A ResponseWriter that records the status and size of the response, for
// the access log. It's an io::ReaderFrom like the server's, which it hands
// the copy to: the files still go out with sendfile(2).
class loggingWriter final : public http::ResponseWriter, public http::Flusher, public io::ReaderFrom {
public:
    explicit loggingWriter(http::ResponseWriter& w) : w_(w) {}

    http::Header& Header() override { return w_.Header(); }
    void WriteHeader(int code) override {
        if (status == 0) status = code;
        w_.WriteHeader(code);
    }
    std::pair<isize, error> Write(std::span<const byte> p) override {
        if (status == 0) status = http::StatusOK;
        auto [n, err] = w_.Write(p);
        size += n;
        return {n, err};
    }
    void Flush() override {
        if (auto* f = dynamic_cast<http::Flusher*>(&w_)) f->Flush();
    }
    std::pair<int64, error> ReadFrom(io::Reader& r) override {
        if (status == 0) status = http::StatusOK;
        auto* rf = dynamic_cast<io::ReaderFrom*>(&w_);
        auto [n, err] = rf ? rf->ReadFrom(r) : io::Copy(w_, r);
        size += n;
        return {n, err};
    }

    int status = 0;
    int64 size = 0;

private:
    http::ResponseWriter& w_;
};

// Logs each request: "GET /index.html 200 1234B 120µs".
ptr<http::Handler> logRequests(ptr<http::Handler> h) {
    return make_ptr<http::HandlerFunc>([h](http::ResponseWriter& w, http::Request& r) {
        time::Time start = time::Now();
        loggingWriter lw(w);
        h->ServeHTTP(lw, r);
        fmt::Println("{} {} {} {} {}B {}", r.RemoteAddr, r.Method, r.URL.Path, lw.status, lw.size, time::Since(start));
    });
}

int main(int argc, char** argv) {
    string dir = ".", addr = ":8080", prefix = "/";
    time::Duration grace = 5 * time::Second;
    flag::StringVar(&dir, "dir", dir, "the directory to serve");
    flag::StringVar(&addr, "addr", addr, "the address to listen on");
    flag::StringVar(&prefix, "prefix", prefix, "the URL path to serve the directory at, such as /files/");
    flag::DurationVar(&grace, "grace", grace, "how long shutting down waits for requests in flight");
    flag::Parse(argc, argv);
    if (!prefix.starts_with("/") || !prefix.ends_with("/")) {
        fmt::Println(os::Stderr, "httpfileserver: -prefix must start and end with a slash");
        return 2;
    }

    auto [root, err] = os::OpenRoot(dir);
    if (err) {
        fmt::Println(os::Stderr, "httpfileserver: {}", err);
        return 1;
    }

    // The file server sees paths relative to the prefix.
    ptr<http::Handler> files = http::FileServerFS(root->FS());
    if (prefix != "/") files = http::StripPrefix(strings::TrimSuffix(prefix, "/"), files);
    auto mux = http::NewServeMux();
    mux->Handle(prefix, files);

    http::Server srv{.Addr = addr, .Handler = logRequests(mux)};
    auto served = make_chan<error>(1);
    go([&srv, served] { served.send(srv.ListenAndServe()); });
    fmt::Println("serving {} at http://{}{}", root->Name(), addr, prefix);

    // Wait for the server to fail, or for Ctrl-C.
    auto [ctx, stop] = signal::NotifyContext(context::Background(), os::Interrupt, syscall::SIGTERM_);
    error serveErr;
    select(
        recv(served, [&](error e) { 
            serveErr = e; 
        }),
        recv(ctx->Done(), [] {})
    );
    stop();
    if (serveErr) {
        fmt::Println(os::Stderr, "httpfileserver: {}", serveErr);
        return 1;
    }

    fmt::Println("shutting down");
    auto [sctx, cancel] = context::WithTimeout(context::Background(), grace);
    if (auto err = srv.Shutdown(sctx)) fmt::Println(os::Stderr, "httpfileserver: shutdown: {}", err);
    cancel();
    served.recv();  // http::ErrServerClosed
    return 0;
}
