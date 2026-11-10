// fetch: a small curl. Fetches a URL and prints the body; HTTP/2 is
// negotiated over TLS, or spoken in the clear with -h2c.
//
//     $ fetch -i https://www.example.com/
//     $ fetch -h2c -X POST -d hello http://localhost:8080/echo
//     $ fetch -n 1000 -c 50 https://localhost:8443/    # load: n requests, c at a time
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

using namespace gopp;

int main(int argc, char** argv) {
    string method = "GET", data;
    bool include = false, insecure = false, h2c = false, http1 = false;
    int64 n = 1, c = 1;
    flag::StringVar(&method, "X", method, "request method");
    flag::StringVar(&data, "d", "", "request body");
    flag::BoolVar(&include, "i", false, "print the status line and response header");
    flag::BoolVar(&insecure, "k", false, "don't verify the server's certificate");
    flag::BoolVar(&h2c, "h2c", false, "HTTP/2 without TLS (prior knowledge)");
    flag::BoolVar(&http1, "http1.1", false, "HTTP/1.1 only");
    flag::Int64Var(&n, "n", 1, "number of requests (bodies are discarded when > 1)");
    flag::Int64Var(&c, "c", 1, "concurrent requests");
    flag::Parse(argc, argv);
    if (flag::Args().size() != 1) {
        fmt::Println(os::Stderr, "usage: fetch [flags] url");
        os::Exit(2);
    }
    string url = flag::Args()[0];

    auto tr = make_ptr<http::Transport>();
    http::Protocols p;
    if (h2c) {
        p.SetUnencryptedHTTP2(true);
    } else if (http1) {
        p.SetHTTP1(true);
    } else {
        p = http::DefaultProtocols();
    }
    tr->Protocols = p;
    if (insecure) tr->TLSClientConfig = tls::Config{.InsecureSkipVerify = true};
    tr->MaxIdleConnsPerHost = static_cast<int>(c);
    http::Client client{.Transport = tr};

    auto fetch = [&](bool print) -> gopp::error {
        ptr<io::Reader> body = data.empty() ? nullptr : strings::NewReader(data);
        auto [req, err] = http::NewRequest(method, url, body);
        if (err) return err;
        auto [resp, derr] = client.Do(req);
        if (derr) return derr;
        if (print && include) {
            fmt::Println("{} {}", resp->Proto, resp->Status);
            for (auto& e : resp->Header) fmt::Println("{}: {}", e.key, e.value);
            fmt::Println("");
        }
        auto [nn, cerr] = io::Copy(print ? os::Stdout : io::Discard(), *resp->Body);
        resp->Body->Close();
        return cerr;
    };

    if (n == 1) {
        if (auto err = fetch(true)) {
            fmt::Println(os::Stderr, "fetch: {}", err);
            return 1;
        }
        return 0;
    }

    // Load: n requests from c fibers.
    std::atomic<int64> next{0}, failed{0};
    time::Time start = time::Now();
    sync::WaitGroup wg;
    for (int64 i = 0; i < c; i++) {
        wg.Go([&] {
            while (next.fetch_add(1) < n) {
                if (auto err = fetch(false)) {
                    if (failed.fetch_add(1) == 0) fmt::Println(os::Stderr, "fetch: {}", err);
                }
            }
        });
    }
    wg.Wait();
    double secs = time::Since(start).Seconds();
    fmt::Println("{} requests, {} failed, {:.3f}s, {:.0f} req/s", n, failed.load(), secs, static_cast<double>(n) / secs);
    return failed.load() ? 1 : 0;
}
