// HTTP/2 end to end: the go++ client against the go++ server, over TLS
// (ALPN h2) and h2c (prior knowledge).
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <set>

using namespace gopp;

namespace {

ptr<http::ServeMux> h2Mux() {
    auto mux = http::NewServeMux();
    mux->HandleFunc("GET /info", [](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "{} {}", r.Proto, r.RemoteAddr);
    });
    mux->HandleFunc("POST /echo", [](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Content-Type", "application/octet-stream");
        io::Copy(w, *r.Body);
    });
    mux->HandleFunc("GET /stream", [](http::ResponseWriter& w, http::Request&) {
        for (int i = 0; i < 3; i++) {
            fmt::Print(w, "chunk {}\n", i);
            dynamic_cast<http::Flusher&>(w).Flush();
            time::Sleep(10 * time::Millisecond);
        }
    });
    mux->HandleFunc("GET /big", [](http::ResponseWriter& w, http::Request&) {
        io::WriteString(w, std::string(3 << 20, 'z'));
    });
    return mux;
}

std::string readBody(const ptr<http::Response>& resp) {
    auto [data, err] = io::ReadAll(*resp->Body);
    EXPECT_FALSE(err) << err;
    resp->Body->Close();
    return std::string(asStringView(data));
}

http::Protocols h2cOnly() {
    http::Protocols p;
    p.SetUnencryptedHTTP2(true);
    return p;
}

// A server speaking h2c (and HTTP/1), and a client speaking only h2c.
struct h2cServer {
    ptr<httptest::Server> ts;
    ptr<http::Transport> tr = make_ptr<http::Transport>();
    http::Client client;

    explicit h2cServer(ptr<http::Handler> h) {
        http::Protocols p;
        p.SetHTTP1(true);
        p.SetUnencryptedHTTP2(true);
        ts = httptest::NewUnstartedServer(std::move(h));
        ts->Config.Protocols = p;
        ts->Start();
        tr->Protocols = h2cOnly();
        client.Transport = tr;
    }
    ~h2cServer() { tr->CloseIdleConnections(); }
};

}  // namespace

TEST(Http2, GetOverTLS) {
    auto ts = httptest::NewTLSServer(h2Mux());
    auto [resp, err] = ts->Client().Get(std::string(ts->URL.view()) + "/info");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(resp->StatusCode, 200);
    EXPECT_EQ(resp->Proto, "HTTP/2.0");
    EXPECT_EQ(resp->ProtoMajor, 2);
    ASSERT_TRUE(resp->TLS);
    EXPECT_EQ(resp->TLS->NegotiatedProtocol, "h2");
    EXPECT_TRUE(readBody(resp).starts_with("HTTP/2.0 ")) << "server saw HTTP/1";
}

TEST(Http2, GetOverH2C) {
    h2cServer s(h2Mux());
    auto [resp, err] = s.client.Get(std::string(s.ts->URL.view()) + "/info");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(resp->Proto, "HTTP/2.0");
    EXPECT_FALSE(resp->TLS);
    EXPECT_TRUE(readBody(resp).starts_with("HTTP/2.0 "));
}

TEST(Http2, MultiplexesOneConnection) {
    // Concurrent requests share one connection: the server sees one
    // remote address.
    auto ts = httptest::NewTLSServer(h2Mux());
    std::string url = std::string(ts->URL.view()) + "/info";
    sync::Mutex mu;
    std::set<std::string> addrs;
    int ok = 0;
    sync::WaitGroup wg;
    // A first request opens the connection the others share.
    auto [first, ferr] = ts->Client().Get(url);
    ASSERT_FALSE(ferr) << ferr;
    addrs.insert(readBody(first));
    for (int i = 0; i < 50; i++) {
        wg.Go([&] {
            auto [resp, err] = ts->Client().Get(url);
            if (err) return;
            std::string b = readBody(resp);
            std::lock_guard l(mu);
            addrs.insert(b);
            ok++;
        });
    }
    wg.Wait();
    EXPECT_EQ(ok, 50);
    EXPECT_EQ(addrs.size(), 1u);
}

TEST(Http2, LargeBodiesBothWays) {
    // Larger than the 64 KiB initial windows and the 1 MiB server stream
    // window, so flow control has to work in both directions.
    h2cServer s(h2Mux());
    std::string data(5 << 20, '\0');
    for (std::size_t i = 0; i < data.size(); i++) data[i] = static_cast<char>(i * 31 + 7);
    auto [resp, err] = s.client.Post(std::string(s.ts->URL.view()) + "/echo", "application/octet-stream",
                                     strings::NewReader(string(data)));
    ASSERT_FALSE(err) << err;
    std::string got = readBody(resp);
    EXPECT_EQ(got.size(), data.size());
    EXPECT_TRUE(got == data);

    auto [big, berr] = s.client.Get(std::string(s.ts->URL.view()) + "/big");
    ASSERT_FALSE(berr) << berr;
    EXPECT_EQ(big->ContentLength, -1);
    EXPECT_EQ(readBody(big).size(), 3u << 20);
}

TEST(Http2, StreamingResponse) {
    auto ts = httptest::NewTLSServer(h2Mux());
    auto [resp, err] = ts->Client().Get(std::string(ts->URL.view()) + "/stream");
    ASSERT_FALSE(err) << err;
    auto br = make_ptr<bufio::Reader>(resp->Body);
    for (int i = 0; i < 3; i++) {
        auto [line, lerr] = br->ReadString('\n');
        ASSERT_FALSE(lerr) << lerr;
        EXPECT_EQ(line, "chunk " + std::to_string(i) + "\n");
    }
    resp->Body->Close();
}

TEST(Http2, ConcurrentEcho) {
    h2cServer s(h2Mux());
    std::string url = std::string(s.ts->URL.view()) + "/echo";
    std::atomic<int> bad{0};
    sync::WaitGroup wg;
    for (int i = 0; i < 32; i++) {
        wg.Go([&, i] {
            for (int j = 0; j < 10; j++) {
                std::string body(static_cast<std::size_t>((i * 7919 + j * 104729) % (200 << 10)), static_cast<char>('a' + i % 26));
                auto [resp, err] = s.client.Post(url, "text/plain", strings::NewReader(string(body)));
                if (err) {
                    bad++;
                    ADD_FAILURE() << err;
                    continue;
                }
                if (readBody(resp) != body) bad++;
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(bad.load(), 0);
}

TEST(Http2, ContextCancelsRequest) {
    auto handler_done = makeChan<bool>(1);
    auto mux = http::NewServeMux();
    mux->HandleFunc("GET /slow", [&](http::ResponseWriter&, http::Request& r) {
        // Ends when the client gives up and the stream is reset.
        r.Context()->Done().recv();
        handler_done.send(true);
    });
    auto ts = httptest::NewTLSServer(mux);
    auto [ctx, cancel] = context::WithTimeout(context::Background(), 50 * time::Millisecond);
    auto [req, rerr] = http::NewRequestWithContext(ctx, "GET", std::string(ts->URL.view()) + "/slow");
    ASSERT_FALSE(rerr);
    auto [resp, err] = ts->Client().Do(req);
    ASSERT_TRUE(err);
    EXPECT_TRUE(errors::Is(err, context::DeadlineExceeded)) << err;
    EXPECT_TRUE(handler_done.recv());
    cancel();
}

TEST(Http2, EarlyBodyCloseResetsStream) {
    // Closing a body before its end cancels the stream; the connection
    // stays usable.
    auto ts = httptest::NewTLSServer(h2Mux());
    auto [resp, err] = ts->Client().Get(std::string(ts->URL.view()) + "/big");
    ASSERT_FALSE(err) << err;
    byte buf[100];
    auto [n, rerr] = io::ReadFull(*resp->Body, buf);
    EXPECT_FALSE(rerr);
    resp->Body->Close();
    auto [r2, err2] = ts->Client().Get(std::string(ts->URL.view()) + "/info");
    ASSERT_FALSE(err2) << err2;
    EXPECT_TRUE(readBody(r2).starts_with("HTTP/2.0"));
}

TEST(Http2, HeadAndNotFound) {
    auto ts = httptest::NewTLSServer(h2Mux());
    auto [resp, err] = ts->Client().Head(std::string(ts->URL.view()) + "/info");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(resp->StatusCode, 200);  // a GET pattern matches HEAD too
    EXPECT_EQ(readBody(resp), "");
    auto [r2, err2] = ts->Client().Get(std::string(ts->URL.view()) + "/nope");
    ASSERT_FALSE(err2) << err2;
    EXPECT_EQ(r2->StatusCode, 404);
    EXPECT_EQ(readBody(r2), "404 page not found\n");
}

TEST(Http2, CloseIdleConnections) {
    // The next request after CloseIdleConnections uses a new connection.
    auto mux = h2Mux();
    auto ts = httptest::NewTLSServer(mux);
    std::string url = std::string(ts->URL.view()) + "/info";
    auto [r1, e1] = ts->Client().Get(url);
    ASSERT_FALSE(e1) << e1;
    std::string a1 = readBody(r1);
    ts->Client().CloseIdleConnections();
    auto [r2, e2] = ts->Client().Get(url);
    ASSERT_FALSE(e2) << e2;
    EXPECT_NE(readBody(r2), a1);
}

TEST(Http2, ServerGoAwayWhileIdle) {
    h2cServer s(h2Mux());
    std::string url = std::string(s.ts->URL.view()) + "/info";
    auto [r1, e1] = s.client.Get(url);
    ASSERT_FALSE(e1) << e1;
    readBody(r1);
    // Shutdown sends GOAWAY and closes the idle connection.
    auto ctx = context::Background();
    EXPECT_FALSE(s.ts->Config.Shutdown(ctx));
    auto [r2, e2] = s.client.Get(url);
    EXPECT_TRUE(e2);  // the server is gone
}
