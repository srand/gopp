#include <gopp/bufio.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/net/http.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace gopp;
using namespace std::chrono_literals;

namespace {

// A server on a loopback port for the duration of a test.
class TestServer {
public:
    explicit TestServer(ptr<http::Handler> h, int max_header_bytes = 0) {
        auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
        EXPECT_FALSE(err) << err;
        addr = ln->Addr();
        srv.Handler = std::move(h);
        srv.MaxHeaderBytes = max_header_bytes;
        done = make_chan<gopp::error>(1);
        go([this, ln] { done.send(srv.Serve(ln)); });
    }

    ~TestServer() {
        srv.Close();
        EXPECT_EQ(done.recv(), http::ErrServerClosed);
    }

    // Sends raw bytes and reads until the server closes the connection.
    std::string roundtrip(std::string_view raw) {
        auto [c, err] = net::Dial("tcp", addr);
        EXPECT_FALSE(err) << err;
        if (err) return {};
        io::WriteString(*c, raw);
        c->SetReadDeadline(time::Now().Add(5 * time::Second));
        auto [data, rerr] = io::ReadAll(*c);
        EXPECT_FALSE(rerr) << rerr;
        return std::string(as_string_view(data));
    }

    std::string get(std::string_view path, std::string_view method = "GET") {
        return roundtrip(std::string(method) + " " + std::string(path) + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    }

    http::Server srv;
    string addr;
    chan<gopp::error> done;
};

std::string body_of(const std::string& resp) {
    auto i = resp.find("\r\n\r\n");
    return i == std::string::npos ? std::string() : resp.substr(i + 4);
}

ptr<http::ServeMux> hello_mux() {
    auto mux = http::NewServeMux();
    mux->HandleFunc("GET /{$}", [](http::ResponseWriter& w, http::Request&) { fmt::Print(w, "Hello, World!\n"); });
    mux->HandleFunc("GET /hello/{name}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "Hello, {}!\n", r.PathValue("name"));
    });
    mux->HandleFunc("GET /files/{path...}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "file={}", r.PathValue("path"));
    });
    mux->HandleFunc("GET /items/{id}", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "item {}", r.PathValue("id"));
    });
    mux->HandleFunc("GET /items/special", [](http::ResponseWriter& w, http::Request&) { fmt::Print(w, "special"); });
    mux->HandleFunc("POST /echo", [](http::ResponseWriter& w, http::Request& r) {
        auto [data, err] = io::ReadAll(*r.Body);
        if (err) {
            http::Error(w, err.Error(), 400);
            return;
        }
        fmt::Print(w, "{}:{}", data.len(), as_string_view(data));
    });
    mux->HandleFunc("/dir/", [](http::ResponseWriter& w, http::Request&) { fmt::Print(w, "dir"); });
    return mux;
}

}  // namespace

TEST(Http, CanonicalHeaderKey) {
    EXPECT_EQ(http::CanonicalHeaderKey("content-type"), "Content-Type");
    EXPECT_EQ(http::CanonicalHeaderKey("X-FORWARDED-for"), "X-Forwarded-For");
    EXPECT_EQ(http::CanonicalHeaderKey("Host"), "Host");
    EXPECT_EQ(http::CanonicalHeaderKey("bad key"), "bad key");
}

TEST(Http, HeaderOps) {
    http::Header h;
    h.Add("accept", "a");
    h.Add("Accept", "b");
    h.Set("content-type", "text/plain");
    EXPECT_EQ(h.Get("ACCEPT"), "a");
    EXPECT_EQ(h.Values("Accept").size(), 2u);
    h.Set("Accept", "c");
    EXPECT_EQ(h.Values("Accept").size(), 1u);
    EXPECT_EQ(h.Get("Accept"), "c");
    h.emplace("X-Y", "z");
    h.Del("content-type");
    EXPECT_FALSE(h.Has("Content-Type"));
    std::string out;
    h.WriteTo(out);
    EXPECT_EQ(out, "Accept: c\r\nX-Y: z\r\n");
}

TEST(Http, DetectContentType) {
    EXPECT_EQ(http::DetectContentType(as_bytes("Hello")), "text/plain; charset=utf-8");
    EXPECT_EQ(http::DetectContentType(as_bytes("  <html><body>")), "text/html; charset=utf-8");
    EXPECT_EQ(http::DetectContentType(as_bytes(std::string_view("\x89PNG\x0D\x0A\x1A\x0A", 8))), "image/png");
    EXPECT_EQ(http::DetectContentType(as_bytes(std::string_view("\x01\x02", 2))), "application/octet-stream");
}

TEST(Http, HelloWorld) {
    TestServer ts(hello_mux());
    std::string resp = ts.get("/");
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200 OK\r\n")) << resp;
    EXPECT_NE(resp.find("Content-Length: 14\r\n"), std::string::npos) << resp;
    EXPECT_NE(resp.find("Content-Type: text/plain; charset=utf-8\r\n"), std::string::npos) << resp;
    EXPECT_NE(resp.find("Date: "), std::string::npos) << resp;
    EXPECT_NE(resp.find("Connection: close\r\n"), std::string::npos) << resp;
    EXPECT_EQ(body_of(resp), "Hello, World!\n");
}

TEST(Http, PathValues) {
    TestServer ts(hello_mux());
    EXPECT_EQ(body_of(ts.get("/hello/gopher")), "Hello, gopher!\n");
    EXPECT_EQ(body_of(ts.get("/hello/caf%C3%A9")), "Hello, caf\xc3\xa9!\n");
    EXPECT_EQ(body_of(ts.get("/files/a/b/c.txt")), "file=a/b/c.txt");
}

TEST(Http, MostSpecificPatternWins) {
    TestServer ts(hello_mux());
    EXPECT_EQ(body_of(ts.get("/items/42")), "item 42");
    EXPECT_EQ(body_of(ts.get("/items/special")), "special");
}

TEST(Http, NotFoundAndMethodNotAllowed) {
    TestServer ts(hello_mux());
    std::string nf = ts.get("/nope");
    EXPECT_TRUE(nf.starts_with("HTTP/1.1 404 Not Found\r\n")) << nf;
    EXPECT_EQ(body_of(nf), "404 page not found\n");
    EXPECT_NE(nf.find("X-Content-Type-Options: nosniff"), std::string::npos);

    std::string na = ts.get("/hello/x", "DELETE");
    EXPECT_TRUE(na.starts_with("HTTP/1.1 405 Method Not Allowed\r\n")) << na;
    EXPECT_NE(na.find("Allow: GET, HEAD\r\n"), std::string::npos) << na;
}

TEST(Http, TrailingSlashAndCleanPathRedirects) {
    TestServer ts(hello_mux());
    std::string r1 = ts.get("/dir");
    EXPECT_TRUE(r1.starts_with("HTTP/1.1 307 Temporary Redirect\r\n")) << r1;
    EXPECT_NE(r1.find("Location: /dir/\r\n"), std::string::npos) << r1;
    std::string r2 = ts.get("/dir/../hello/x");
    EXPECT_TRUE(r2.starts_with("HTTP/1.1 307")) << r2;
    EXPECT_NE(r2.find("Location: /hello/x\r\n"), std::string::npos) << r2;
    EXPECT_EQ(body_of(ts.get("/dir/sub/thing")), "dir");
}

TEST(Http, HeadHasNoBody) {
    TestServer ts(hello_mux());
    std::string r = ts.get("/", "HEAD");
    EXPECT_TRUE(r.starts_with("HTTP/1.1 200 OK\r\n")) << r;
    EXPECT_EQ(body_of(r), "");
}

TEST(Http, PostBody) {
    TestServer ts(hello_mux());
    std::string r = ts.roundtrip("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    EXPECT_EQ(body_of(r), "5:hello") << r;
}

TEST(Http, ChunkedRequestBody) {
    TestServer ts(hello_mux());
    std::string r = ts.roundtrip(
        "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
        "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n");
    EXPECT_EQ(body_of(r), "11:hello world") << r;
}

TEST(Http, ChunkedResponse) {
    auto mux = http::NewServeMux();
    mux->HandleFunc("/", [](http::ResponseWriter& w, http::Request&) {
        std::string piece(500, 'x');
        for (int i = 0; i < 10; i++) io::WriteString(w, piece);
    });
    mux->HandleFunc("/big", [](http::ResponseWriter& w, http::Request&) {
        io::WriteString(w, std::string(5000, 'y'));
    });
    TestServer ts(mux);
    std::string r = ts.get("/");
    EXPECT_NE(r.find("Transfer-Encoding: chunked\r\n"), std::string::npos) << r.substr(0, 300);
    EXPECT_EQ(r.find("Content-Length"), std::string::npos);
    std::string body = body_of(r);
    // Small writes fill the 2048-byte buffer before the first chunk.
    EXPECT_TRUE(body.starts_with("800\r\n")) << body.substr(0, 20);
    EXPECT_TRUE(body.ends_with("0\r\n\r\n"));
    // A large write to an empty buffer is one chunk, as with bufio.Writer.
    EXPECT_TRUE(body_of(ts.get("/big")).starts_with("1388\r\n"));
}

TEST(Http, ExplicitContentLengthAndStatus) {
    auto mux = http::NewServeMux();
    mux->HandleFunc("/", [](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Content-Type", "application/json");
        w.Header().Set("Content-Length", "2");
        w.WriteHeader(http::StatusCreated);
        io::WriteString(w, "{}");
    });
    TestServer ts(mux);
    std::string r = ts.get("/");
    EXPECT_TRUE(r.starts_with("HTTP/1.1 201 Created\r\n")) << r;
    EXPECT_NE(r.find("Content-Type: application/json\r\n"), std::string::npos);
    EXPECT_NE(r.find("Content-Length: 2\r\n"), std::string::npos);
    EXPECT_EQ(body_of(r), "{}");
}

TEST(Http, KeepAlive) {
    TestServer ts(hello_mux());
    auto [c, err] = net::Dial("tcp", ts.addr);
    ASSERT_FALSE(err);
    c->SetReadDeadline(time::Now().Add(5 * time::Second));
    bufio::Reader br(c);
    for (int i = 0; i < 3; i++) {
        fmt::Print(*c, "GET /hello/n{} HTTP/1.1\r\nHost: t\r\n\r\n", i);
        auto [status, serr] = br.ReadString('\n');
        ASSERT_FALSE(serr) << serr;
        EXPECT_EQ(status, "HTTP/1.1 200 OK\r\n");
        int64 len = -1;
        for (;;) {
            auto [line, lerr] = br.ReadString('\n');
            ASSERT_FALSE(lerr);
            if (line == "\r\n") break;
            if (line.starts_with("Content-Length: ")) len = strconv::Atoi(strings::TrimSpace(line.slice(16)).view()).first;
        }
        ASSERT_GT(len, 0);
        std::string body(static_cast<std::size_t>(len), '\0');
        io::ReadFull(br, std::span<byte>(reinterpret_cast<byte*>(body.data()), body.size()));
        EXPECT_EQ(body, fmt::Sprint("Hello, n{}!\n", i));
    }
}

TEST(Http, Pipelining) {
    TestServer ts(hello_mux());
    std::string r = ts.roundtrip(
        "GET /hello/a HTTP/1.1\r\nHost: t\r\n\r\n"
        "GET /hello/b HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    EXPECT_NE(r.find("Hello, a!"), std::string::npos) << r;
    EXPECT_NE(r.find("Hello, b!"), std::string::npos) << r;
}

TEST(Http, Http10) {
    TestServer ts(hello_mux());
    std::string r = ts.roundtrip("GET / HTTP/1.0\r\n\r\n");
    EXPECT_TRUE(r.starts_with("HTTP/1.0 200 OK\r\n")) << r;
    EXPECT_EQ(body_of(r), "Hello, World!\n");
}

TEST(Http, BadRequests) {
    TestServer ts(hello_mux());
    EXPECT_TRUE(ts.roundtrip("garbage\r\n\r\n").starts_with("HTTP/1.1 400 Bad Request")) ;
    std::string nohost = ts.roundtrip("GET / HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(nohost.starts_with("HTTP/1.1 400 Bad Request")) << nohost;
    EXPECT_NE(nohost.find("missing required Host header"), std::string::npos);
    std::string te = ts.roundtrip("POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip\r\n\r\n");
    EXPECT_TRUE(te.starts_with("HTTP/1.1 501 Not Implemented")) << te;
}

TEST(Http, HeaderTooLarge) {
    TestServer ts(hello_mux(), 1024);
    std::string big = "GET / HTTP/1.1\r\nHost: t\r\nX-Big: " + std::string(8192, 'a') + "\r\n\r\n";
    std::string r = ts.roundtrip(big);
    EXPECT_TRUE(r.starts_with("HTTP/1.1 431 Request Header Fields Too Large")) << r;
}

TEST(Http, FlusherAndContext) {
    auto mux = http::NewServeMux();
    mux->HandleFunc("GET /events", [](http::ResponseWriter& w, http::Request& r) {
        auto* f = dynamic_cast<http::Flusher*>(&w);
        if (!f) {
            http::Error(w, "streaming unsupported", 500);
            return;
        }
        w.Header().Set("Content-Type", "text/event-stream");
        for (int i = 0; i < 3 && !r.Context()->Err(); i++) {
            fmt::Print(w, "data: {}\n\n", i);
            f->Flush();
        }
    });
    TestServer ts(mux);
    auto [c, err] = net::Dial("tcp", ts.addr);
    ASSERT_FALSE(err);
    io::WriteString(*c, "GET /events HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    c->SetReadDeadline(time::Now().Add(5 * time::Second));
    auto [data, rerr] = io::ReadAll(*c);
    std::string r(as_string_view(data));
    EXPECT_NE(r.find("Transfer-Encoding: chunked"), std::string::npos) << r;
    EXPECT_NE(r.find("data: 0\n\n"), std::string::npos) << r;
    EXPECT_NE(r.find("data: 2\n\n"), std::string::npos) << r;
}

TEST(Http, HandlerExceptionDropsConnection) {
    auto mux = http::NewServeMux();
    mux->HandleFunc("/", [](http::ResponseWriter&, http::Request&) { throw std::runtime_error("boom"); });
    TestServer ts(mux);
    EXPECT_EQ(ts.get("/"), "");
}

TEST(Http, ConcurrentClients) {
    TestServer ts(hello_mux());
    sync::WaitGroup wg;
    std::atomic<int> ok{0};
    for (int i = 0; i < 50; i++) {
        wg.Go([&, i] {
            if (body_of(ts.get(fmt::Sprint("/hello/c{}", i).view())) == fmt::Sprint("Hello, c{}!\n", i).view()) ok++;
        });
    }
    wg.Wait();
    EXPECT_EQ(ok.load(), 50);
}

TEST(Http, Shutdown) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    http::Server srv{.Handler = hello_mux()};
    auto done = make_chan<gopp::error>(1);
    go([&, ln] { done.send(srv.Serve(ln)); });
    time::Sleep(5ms);
    auto [ctx, cancel] = context::WithTimeout(context::Background(), time::Second);
    EXPECT_FALSE(srv.Shutdown(ctx));
    EXPECT_EQ(done.recv(), http::ErrServerClosed);
    cancel();
}

TEST(Http, DuplicatePatternPanics) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");  // the runtime has threads
    EXPECT_DEATH(
        {
            http::ServeMux m;
            m.HandleFunc("/a", [](http::ResponseWriter&, http::Request&) {});
            m.HandleFunc("/a", [](http::ResponseWriter&, http::Request&) {});
        },
        "conflicts with pattern");
}

// ---------------------------------------------------------------------------
// Client

namespace {

ptr<http::ServeMux> client_mux() {
    auto mux = hello_mux();
    mux->HandleFunc("GET /addr", [](http::ResponseWriter& w, http::Request& r) { fmt::Print(w, "{}", r.RemoteAddr); });
    mux->HandleFunc("GET /big", [](http::ResponseWriter& w, http::Request&) { io::WriteString(w, std::string(5000, 'z')); });
    mux->HandleFunc("GET /slow", [](http::ResponseWriter& w, http::Request&) {
        time::Sleep(300ms);
        fmt::Print(w, "late");
    });
    mux->HandleFunc("GET /loop", [](http::ResponseWriter& w, http::Request& r) { http::Redirect(w, r, "/loop", 302); });
    return mux;
}

std::string read_body(const ptr<http::Response>& resp) {
    auto [data, err] = io::ReadAll(*resp->Body);
    EXPECT_FALSE(err) << err;
    resp->Body->Close();
    return std::string(as_string_view(data));
}

}  // namespace

TEST(HttpClient, Get) {
    TestServer ts(client_mux());
    auto [resp, err] = http::Get(fmt::Sprint("http://{}/", ts.addr).view());
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(resp->StatusCode, 200);
    EXPECT_EQ(resp->Status, "200 OK");
    EXPECT_EQ(resp->Proto, "HTTP/1.1");
    EXPECT_EQ(resp->ContentLength, 14);
    EXPECT_EQ(resp->Header.Get("Content-Type"), "text/plain; charset=utf-8");
    EXPECT_EQ(read_body(resp), "Hello, World!\n");
    EXPECT_EQ(resp->Request->Method, "GET");
}

TEST(HttpClient, ReusesConnections) {
    TestServer ts(client_mux());
    auto t = make_ptr<http::Transport>();
    http::Client c{.Transport = t};
    std::string first;
    for (int i = 0; i < 5; i++) {
        auto [resp, err] = c.Get(fmt::Sprint("http://{}/addr", ts.addr).view());
        ASSERT_FALSE(err) << err;
        std::string addr = read_body(resp);
        if (i == 0) first = addr;
        EXPECT_EQ(addr, first) << "request " << i << " used a new connection";
    }
}

TEST(HttpClient, ClosingUnreadBodyDropsConnection) {
    TestServer ts(client_mux());
    auto t = make_ptr<http::Transport>();
    http::Client c{.Transport = t};
    auto [r1, e1] = c.Get(fmt::Sprint("http://{}/big", ts.addr).view());
    ASSERT_FALSE(e1);
    r1->Body->Close();
    auto [r2, e2] = c.Get(fmt::Sprint("http://{}/big", ts.addr).view());
    ASSERT_FALSE(e2) << e2;
    EXPECT_EQ(read_body(r2), std::string(5000, 'z'));  // chunked
    EXPECT_EQ(r2->ContentLength, -1);
}

TEST(HttpClient, Post) {
    TestServer ts(client_mux());
    auto [resp, err] = http::Post(fmt::Sprint("http://{}/echo", ts.addr).view(), "text/plain", strings::NewReader("hello"));
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(read_body(resp), "5:hello");

    // A body of unknown length is sent chunked.
    struct reader final : io::Reader {
        int n = 0;
        std::pair<isize, gopp::error> Read(std::span<byte> p) override {
            if (n++ == 3) return {0, io::EOF_};
            p[0] = 'a';
            return {1, nullptr};
        }
    };
    auto [resp2, err2] = http::Post(fmt::Sprint("http://{}/echo", ts.addr).view(), "text/plain", make_ptr<reader>());
    ASSERT_FALSE(err2) << err2;
    EXPECT_EQ(read_body(resp2), "3:aaa");
}

TEST(HttpClient, HeadAndNotFound) {
    TestServer ts(client_mux());
    auto [h, herr] = http::Head(fmt::Sprint("http://{}/", ts.addr).view());
    ASSERT_FALSE(herr) << herr;
    EXPECT_EQ(h->StatusCode, 200);
    EXPECT_EQ(h->ContentLength, 14);
    EXPECT_EQ(read_body(h), "");

    auto [nf, nerr] = http::Get(fmt::Sprint("http://{}/nope", ts.addr).view());
    ASSERT_FALSE(nerr);
    EXPECT_EQ(nf->StatusCode, 404);
    EXPECT_EQ(read_body(nf), "404 page not found\n");
}

TEST(HttpClient, FollowsRedirects) {
    TestServer ts(client_mux());
    auto [resp, err] = http::Get(fmt::Sprint("http://{}/dir", ts.addr).view());
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(resp->StatusCode, 200);
    EXPECT_EQ(read_body(resp), "dir");
    EXPECT_EQ(resp->Request->URL.Path, "/dir/");

    auto [loop, lerr] = http::Get(fmt::Sprint("http://{}/loop", ts.addr).view());
    ASSERT_TRUE(lerr);
    EXPECT_NE(std::string(lerr.Error().view()).find("stopped after 10 redirects"), std::string::npos) << lerr;
}

TEST(HttpClient, Errors) {
    auto [ln, lerr] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(lerr);
    string addr = ln->Addr();
    ln->Close();
    auto [r, err] = http::Get(fmt::Sprint("http://{}/", addr).view());
    ASSERT_TRUE(err);
    EXPECT_TRUE(err.Error().starts_with(fmt::Sprint("Get \"http://{}/\": dial tcp", addr).view())) << err;
    EXPECT_TRUE(errors::As<url::Error>(err));

    auto [r2, err2] = http::Get("ftp://example.com/");
    ASSERT_TRUE(err2);
    EXPECT_NE(std::string(err2.Error().view()).find("unsupported protocol scheme \"ftp\""), std::string::npos) << err2;
}

TEST(HttpClient, Timeout) {
    TestServer ts(client_mux());
    http::Client c{.Timeout = 50ms};
    auto start = time::Now();
    auto [resp, err] = c.Get(fmt::Sprint("http://{}/slow", ts.addr).view());
    ASSERT_TRUE(err);
    EXPECT_TRUE(err->Timeout()) << err;
    EXPECT_LT(time::Since(start).Milliseconds(), 250);
}

TEST(HttpClient, ConcurrentGets) {
    TestServer ts(client_mux());
    auto t = make_ptr<http::Transport>();
    t->MaxIdleConnsPerHost = 16;
    http::Client c{.Transport = t};
    sync::WaitGroup wg;
    std::atomic<int> ok{0};
    for (int i = 0; i < 16; i++) {
        wg.Go([&, i] {
            for (int j = 0; j < 50; j++) {
                auto [resp, err] = c.Get(fmt::Sprint("http://{}/hello/c{}x{}", ts.addr, i, j).view());
                if (err) return;
                if (read_body(resp) == fmt::Sprint("Hello, c{}x{}!\n", i, j).view()) ok++;
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(ok.load(), 16 * 50);
}
