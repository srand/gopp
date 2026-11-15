// Ported from Go 1.27.3's net/http: the tests of trailers in
// clientserver_test.go, of ResponseController in responsecontroller_test.go
// and of hijacking in serve_test.go, run over HTTP/1.1, HTTP/1.1 over TLS
// and HTTP/2 where Go's run in those modes, with Go's test names. Left out:
// TestWrappedResponseController (go++ has no Unwrap).
//
// Adapted: Go's Trailer holds the announced keys with nil values before
// the body is read; go++'s Header can't hold a key without a value, so the
// Trailer is empty until then.
#include "net/http/internal.hpp"

#include <gopp/bufio.hpp>
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>

using namespace gopp;

// Go's test modes. (Not in the anonymous namespace: googletest's
// parameterized tests hold one.)
enum class clientServerTestMode { http1, https1, http2 };

namespace {

using testMode = clientServerTestMode;

std::string modeName(const testing::TestParamInfo<testMode>& info) {
    switch (info.param) {
    case testMode::http1: return "h1";
    case testMode::https1: return "https1";
    case testMode::http2: return "h2";
    }
    return "";
}

std::string str(const string& s) { return std::string(s.view()); }

// Go's eofReaderFunc: runs f at EOF.
struct eofReaderFunc final : io::Reader {
    explicit eofReaderFunc(std::function<void()> f) : f_(std::move(f)) {}
    std::pair<isize, error> Read(std::span<byte>) override {
        f_();
        return {0, io::EOF_};
    }
    std::function<void()> f_;
};

// Go's wantBody: the body is want, read to the end.
void wantBody(const ptr<http::Response>& res, std::string_view want) {
    ASSERT_TRUE(res);
    auto [slurp, err] = io::ReadAll(*res->Body);
    ASSERT_FALSE(err) << "error reading body: " << err;
    EXPECT_EQ(std::string(asStringView(slurp)), want) << "body";
    res->Body->Close();
}

class GoNetHttpClientServer : public testing::TestWithParam<testMode> {
protected:
    // newClientServerTest: a test server in the mode, closed at the end.
    httptest::Server& serve(http::handlerFn f) {
        auto h = make_ptr<http::HandlerFunc>(std::move(f));
        http::Protocols p;
        switch (GetParam()) {
        case testMode::http1: ts_ = httptest::NewServer(std::move(h)); break;
        case testMode::https1:
            p.SetHTTP1(true);
            ts_ = httptest::NewTLSServer(std::move(h), p);
            break;
        case testMode::http2:
            p.SetHTTP2(true);
            ts_ = httptest::NewTLSServer(std::move(h), p);
            break;
        }
        return *ts_;
    }
    void TearDown() override {
        if (ts_) ts_->Close();
    }
    ptr<httptest::Server> ts_;
};

INSTANTIATE_TEST_SUITE_P(Modes, GoNetHttpClientServer,
                         testing::Values(testMode::http1, testMode::https1, testMode::http2), modeName);

// The keys of a Header, sorted and without duplicates.
std::vector<std::string> keys(const http::Header& h) {
    std::vector<std::string> out;
    for (auto& e : h) {
        if (std::find(out.begin(), out.end(), str(e.key)) == out.end()) out.push_back(str(e.key));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); i++) out += (i ? " " : "") + v[i];
    return out + "]";
}

}  // namespace

// Clients send trailers to a server, which reads them.
TEST_P(GoNetHttpClientServer, TrailersClientToServer) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) {
        auto [slurp, err] = io::ReadAll(*r.Body);
        EXPECT_FALSE(err) << "Server reading request body: " << err;
        EXPECT_EQ(std::string(asStringView(slurp)), "foo") << "Server read request body";
        if (r.Trailer.empty()) {
            io::WriteString(w, "nil Trailer");
        } else {
            io::WriteString(w, "decl: " + join(keys(r.Trailer)) + ", vals: " + str(r.Trailer.Get("Client-Trailer-A")) + ", " +
                                   str(r.Trailer.Get("Client-Trailer-B")));
        }
    });

    ptr<http::Request> req;
    auto body = io::MultiReader({
        make_ptr<eofReaderFunc>([&] { req->Trailer.Set("Client-Trailer-A", "valuea"); }),
        strings::NewReader("foo"),
        make_ptr<eofReaderFunc>([&] { req->Trailer.Set("Client-Trailer-B", "valueb"); }),
    });
    std::tie(req, std::ignore) = http::NewRequest("POST", ts.URL, body);
    // Announced now, set later.
    req->Trailer.Set("Client-Trailer-A", "");
    req->Trailer.Set("Client-Trailer-B", "");
    req->ContentLength = -1;
    auto [res, err] = ts.Client().Do(req);
    ASSERT_FALSE(err) << err;
    wantBody(res, "decl: [Client-Trailer-A Client-Trailer-B], vals: valuea, valueb");
}

namespace {

void testTrailersServerToClient(GoNetHttpClientServer& t, httptest::Server& ts, testMode mode, bool flush) {
    const std::string body = "Some body";
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << err;

    int64 wantLen = -1;
    http::Header wantHeader;
    wantHeader.Set("Content-Type", "text/plain; charset=utf-8");
    if (mode == testMode::http2 && !flush) {
        // HTTP/1.1's trailers need chunking, and a flush at the first
        // write; HTTP/2's framing doesn't, and the server knows the length.
        wantLen = static_cast<int64>(body.size());
        wantHeader.Set("Content-Length", strconv::Itoa(wantLen));
    }
    EXPECT_EQ(res->ContentLength, wantLen) << "ContentLength";

    res->Header.Del("Date");  // irrelevant for the test
    EXPECT_EQ(keys(res->Header), keys(wantHeader)) << "Header";
    for (auto& e : wantHeader) EXPECT_EQ(res->Header.Get(e.key), e.value) << e.key;

    EXPECT_TRUE(res->Trailer.empty()) << "Trailer before body read";

    wantBody(res, body);

    EXPECT_EQ(keys(res->Trailer), (std::vector<std::string>{"Server-Trailer-A", "Server-Trailer-C"}));
    EXPECT_EQ(res->Trailer.Get("Server-Trailer-A"), "valuea");
    EXPECT_EQ(res->Trailer.Get("Server-Trailer-C"), "valuec");
}

http::handlerFn trailersServerToClientHandler(bool flush) {
    return [flush](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Trailer", "Server-Trailer-A, Server-Trailer-B");
        w.Header().Add("Trailer", "Server-Trailer-C");

        io::WriteString(w, "Some body");
        if (flush) dynamic_cast<http::Flusher&>(w).Flush();

        // How handlers set trailers: declare them ahead of time with the
        // Trailer header, and set their values after the response has been
        // written.
        w.Header().Set("Server-Trailer-A", "valuea");
        w.Header().Set("Server-Trailer-C", "valuec");  // skipping B
        w.Header().Set("Server-Trailer-NotDeclared", "should be omitted");
    };
}

}  // namespace

// Servers send trailers to a client, which reads them.
TEST_P(GoNetHttpClientServer, TrailersServerToClient) {
    auto& ts = serve(trailersServerToClientHandler(false));
    testTrailersServerToClient(*this, ts, GetParam(), false);
}

TEST_P(GoNetHttpClientServer, TrailersServerToClientFlush) {
    auto& ts = serve(trailersServerToClientHandler(true));
    testTrailersServerToClient(*this, ts, GetParam(), true);
}

// Trailers set with TrailerPrefix after the header was written, without
// being declared (Go's TestServerUndeclaredTrailers, in serve_test.go).
TEST_P(GoNetHttpClientServer, ServerUndeclaredTrailers) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Foo", "Bar");
        w.Header().Set("Trailer:Foo", "Baz");
        dynamic_cast<http::Flusher&>(w).Flush();
        w.Header().Add("Trailer:Foo", "Baz2");
        w.Header().Set("Trailer:Bar", "Quux");
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << err;
    auto [_, rerr] = io::ReadAll(*res->Body);
    ASSERT_FALSE(rerr) << rerr;
    res->Body->Close();
    res->Header.Del("Date");
    res->Header.Del("Content-Type");
    res->Header.Del("Content-Length");
    EXPECT_EQ(keys(res->Header), (std::vector<std::string>{"Foo"}));
    EXPECT_EQ(res->Header.Get("Foo"), "Bar");
    EXPECT_EQ(keys(res->Trailer), (std::vector<std::string>{"Bar", "Foo"}));
    EXPECT_EQ(res->Trailer.Values("Foo"), (std::vector<string>{"Baz", "Baz2"}));
    EXPECT_EQ(res->Trailer.Get("Bar"), "Quux");
}

// httptest's ResponseRecorder reports the trailers too.
TEST(GoNetHttpClientServerRecorder, RecorderTrailers) {
    auto rr = httptest::NewRecorder();
    auto h = trailersServerToClientHandler(false);
    h(*rr, *httptest::NewRequest("GET", "/"));
    auto res = rr->Result();
    EXPECT_EQ(keys(res->Trailer), (std::vector<std::string>{"Server-Trailer-A", "Server-Trailer-C"}));
    EXPECT_EQ(res->Trailer.Get("Server-Trailer-A"), "valuea");
}

// ---------------------------------------------------------------------------
// ResponseController (responsecontroller_test.go)

namespace {

// Go's neverEnding: an endless stream of one byte.
struct neverEnding final : io::Reader {
    explicit neverEnding(byte b) : b_(b) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        std::fill(p.begin(), p.end(), b_);
        return {static_cast<isize>(p.size()), nullptr};
    }
    byte b_;
};

}  // namespace

TEST_P(GoNetHttpClientServer, ResponseControllerFlush) {
    auto continuec = make_chan<empty>();
    auto& ts = serve([continuec](http::ResponseWriter& w, http::Request&) {
        http::ResponseController ctl(w);
        io::WriteString(w, "one");
        if (error err = ctl.Flush()) {
            ADD_FAILURE() << "ctl.Flush() = " << err << ", want nil";
            return;
        }
        continuec.recv();
        io::WriteString(w, "two");
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << "unexpected connection error: " << err;
    byte buf[16];
    auto [n, rerr] = res->Body->Read(buf);
    continuec.close();
    ASSERT_TRUE(!rerr && std::string_view(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n)) == "one")
        << "Body.Read = " << rerr;
    auto [got, aerr] = io::ReadAll(*res->Body);
    EXPECT_TRUE(!aerr && asStringView(got) == "two") << "Body.Read = " << aerr;
    res->Body->Close();
}

TEST_P(GoNetHttpClientServer, ResponseControllerHijack) {
    testMode mode = GetParam();
    auto& ts = serve([mode](http::ResponseWriter& w, http::Request&) {
        http::ResponseController ctl(w);
        auto [c, _, err] = ctl.Hijack();
        if (mode == testMode::http2) {
            EXPECT_TRUE(err) << "ctl.Hijack = nil, want error";
            w.Header().Set("X-Header", "set");
            return;
        }
        if (err) {
            ADD_FAILURE() << "ctl.Hijack = " << err << ", want nil";
            return;
        }
        io::WriteString(*c, "HTTP/1.0 200 OK\r\nX-Header: set\r\nContent-Length: 0\r\n\r\n");
        c->Close();
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(res->Header.Get("X-Header"), "set");
    res->Body->Close();
}

TEST_P(GoNetHttpClientServer, ResponseControllerSetPastWriteDeadline) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request&) {
        http::ResponseController ctl(w);
        io::WriteString(w, "one");
        EXPECT_FALSE(ctl.Flush()) << "before setting deadline";
        EXPECT_FALSE(ctl.SetWriteDeadline(time::Now().Add(-10 * time::Second)));
        io::WriteString(w, "two");
        EXPECT_TRUE(ctl.Flush()) << "after setting deadline: ctl.Flush() = nil, want non-nil";
        // Connection errors are sticky: a new deadline doesn't undo them.
        EXPECT_FALSE(ctl.SetWriteDeadline(time::Now().Add(time::Hour)));
        io::WriteString(w, "three");
        EXPECT_TRUE(ctl.Flush()) << "after resetting deadline: ctl.Flush() = nil, want non-nil";
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << "unexpected connection error: " << err;
    auto [b, _] = io::ReadAll(*res->Body);
    EXPECT_EQ(asStringView(b), "one") << "unexpected body";
    res->Body->Close();
}

TEST_P(GoNetHttpClientServer, ResponseControllerSetFutureWriteDeadline) {
    auto errc = make_chan<error>(1);
    auto startwritec = make_chan<empty>();
    auto& ts = serve([errc, startwritec](http::ResponseWriter& w, http::Request&) {
        http::ResponseController ctl(w);
        w.WriteHeader(200);
        EXPECT_FALSE(ctl.Flush());
        startwritec.recv();  // not until the client has the response header
        EXPECT_FALSE(ctl.SetWriteDeadline(time::Now().Add(time::Millisecond)));
        neverEnding src('a');
        auto [_, err] = io::Copy(w, src);
        errc.send(err);
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    startwritec.close();
    ASSERT_FALSE(err) << "unexpected connection error: " << err;
    auto [_, cerr] = io::Copy(io::Discard(), *res->Body);
    EXPECT_TRUE(cerr) << "client reading from truncated response body: got nil error, want non-nil";
    res->Body->Close();
    error serr = errc.recv();
    EXPECT_TRUE(errors::Is(serr, os::ErrDeadlineExceeded)) << "server timed out writing response body: got " << serr;
}

TEST_P(GoNetHttpClientServer, ResponseControllerSetPastReadDeadline) {
    auto readc = make_chan<empty>();
    auto donec = make_chan<empty>();
    auto& ts = serve([readc, donec](http::ResponseWriter& w, http::Request& r) {
        struct closer {
            chan<empty> c;
            ~closer() { c.close(); }
        } closeDone{donec};
        http::ResponseController ctl(w);
        byte b[3];
        auto [n, err] = io::ReadFull(*r.Body, b);
        if (err || std::string_view(reinterpret_cast<char*>(b), static_cast<std::size_t>(n)) != "one") {
            ADD_FAILURE() << "before setting read deadline: Read = " << err;
            return;
        }
        EXPECT_FALSE(ctl.SetReadDeadline(time::Now()));
        auto [rest, rerr] = io::ReadAll(*r.Body);
        EXPECT_TRUE(rerr && rest.empty()) << "after setting read deadline: Read = " << std::string(asStringView(rest)) << ", nil, want error";
        readc.close();
        // Connection errors are sticky.
        EXPECT_FALSE(ctl.SetReadDeadline(time::Time{}));
        auto [rest2, rerr2] = io::ReadAll(*r.Body);
        EXPECT_TRUE(rerr2) << "after resetting read deadline: Read = " << std::string(asStringView(rest2)) << ", nil, want error";
    });

    auto [pr, pw] = io::Pipe();
    sync::WaitGroup wg;
    wg.Go([pw, readc, donec] {
        io::WriteString(*pw, "one");
        bool gotRead = false;
        select(recv(readc, [&] { gotRead = true; }), recv(donec, [&] {
                   bool closed = false;
                   empty e;
                   readc.tryRecv(e, &closed);
                   gotRead = closed;
               }));
        if (!gotRead) ADD_FAILURE() << "server handler unexpectedly exited without closing readc";
        else io::WriteString(*pw, "two");
        pw->Close();
    });
    auto [res, err] = ts.Client().Post(ts.URL, "text/foo", pr);
    if (!err) res->Body->Close();
    wg.Wait();
}

TEST_P(GoNetHttpClientServer, ResponseControllerSetFutureReadDeadline) {
    const std::string respBody = "response body";
    auto& ts = serve([respBody](http::ResponseWriter& w, http::Request& req) {
        http::ResponseController ctl(w);
        EXPECT_FALSE(ctl.SetReadDeadline(time::Now().Add(time::Millisecond)));
        auto [_, err] = io::Copy(io::Discard(), *req.Body);
        EXPECT_TRUE(errors::Is(err, os::ErrDeadlineExceeded)) << "server timed out reading request body: got " << err;
        io::WriteString(w, respBody);
    });
    auto [pr, pw] = io::Pipe();
    auto [res, err] = ts.Client().Post(ts.URL, "text/apocryphal", pr);
    ASSERT_FALSE(err) << err;
    auto [got, rerr] = io::ReadAll(*res->Body);
    EXPECT_TRUE(asStringView(got) == respBody && !rerr) << "client read response body: " << rerr;
    res->Body->Close();
    pw->Close();
}

TEST_P(GoNetHttpClientServer, ResponseControllerEnableFullDuplex) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& req) {
        http::ResponseController ctl(w);
        EXPECT_FALSE(ctl.EnableFullDuplex());
        w.WriteHeader(200);
        ctl.Flush();
        for (;;) {
            byte buf[1];
            auto [n, err] = req.Body->Read(buf);
            if (n != 1 || err) break;
            w.Write(buf);
            ctl.Flush();
        }
    });
    auto [pr, pw] = io::Pipe();
    auto [res, err] = ts.Client().Post(ts.URL, "text/apocryphal", pr);
    ASSERT_FALSE(err) << err;
    for (byte i = 0; i < 10; i++) {
        byte b[1] = {i};
        ASSERT_FALSE(pw->Write(b).second) << "Write";
        byte got[1];
        auto [n, rerr] = res->Body->Read(got);
        ASSERT_TRUE(n == 1 && !rerr) << "Read: " << n << ", " << rerr;
        ASSERT_EQ(got[0], i) << "read byte";
    }
    pw->Close();
    res->Body->Close();
}

// ---------------------------------------------------------------------------
// Hijacking (serve_test.go), over HTTP/1

namespace {

// A server of h, and a client connection to it that has sent request.
struct rawConnTest {
    explicit rawConnTest(http::handlerFn h) : ts(httptest::NewServer(make_ptr<http::HandlerFunc>(std::move(h)))) {
        auto [c, err] = net::Dial("tcp", ts->Listener->Addr()->String());
        EXPECT_FALSE(err) << err;
        conn = c;
    }
    ~rawConnTest() {
        if (conn) conn->Close();
        ts->Close();
    }
    ptr<httptest::Server> ts;
    ptr<net::Conn> conn;
};

}  // namespace

TEST(GoNetHttpServe, WriteAfterHijack) {
    auto wrotec = make_chan<bool>(1);
    rawConnTest t([wrotec](http::ResponseWriter& rw, http::Request&) {
        auto [conn, bufrw, err] = dynamic_cast<http::Hijacker&>(rw).Hijack();
        if (err) {
            ADD_FAILURE() << err;
            return;
        }
        go([conn, bufrw, wrotec] {
            bufrw->Write(as_bytes("[hijack-to-bufw]"));
            bufrw->Flush();
            conn->Write(as_bytes("[hijack-to-conn]"));
            conn->Close();
            wrotec.send(true);
        });
    });
    io::WriteString(*t.conn, "GET / HTTP/1.1\r\nHost: golang.org\r\n\r\n");
    auto [got, err] = io::ReadAll(*t.conn);
    wrotec.recv();
    EXPECT_EQ(asStringView(got), "[hijack-to-bufw][hijack-to-conn]");
}

TEST(GoNetHttpServe, DoubleHijack) {
    auto done = make_chan<empty>();
    rawConnTest t([done](http::ResponseWriter& rw, http::Request&) {
        auto [conn, _, err] = dynamic_cast<http::Hijacker&>(rw).Hijack();
        if (err) {
            ADD_FAILURE() << err;
            return;
        }
        auto [c2, _2, err2] = dynamic_cast<http::Hijacker&>(rw).Hijack();
        EXPECT_EQ(err2, http::ErrHijacked) << "got err = nil; want err != nil";
        conn->Close();
        done.close();
    });
    io::WriteString(*t.conn, "GET / HTTP/1.1\r\nHost: golang.org\r\n\r\n");
    done.recv();
}

// The hijacker's Reader has what the client sent after the request.
TEST(GoNetHttpServe, ServerHijackGetsFullBody) {
    auto done = make_chan<empty>();
    const std::string needle(100 * 1024, 'x');  // larger than the server's buffer
    rawConnTest t([&](http::ResponseWriter& w, http::Request&) {
        struct closer {
            chan<empty> c;
            ~closer() { c.close(); }
        } closeDone{done};
        auto [conn, buf, err] = dynamic_cast<http::Hijacker&>(w).Hijack();
        if (err) {
            ADD_FAILURE() << err;
            return;
        }
        std::string got(needle.size(), '\0');
        auto [n, rerr] = io::ReadFull(*buf->Reader, std::span<byte>(reinterpret_cast<byte*>(got.data()), got.size()));
        EXPECT_TRUE(static_cast<std::size_t>(n) == needle.size() && got == needle && !rerr) << "ReadFull = " << n << ", " << rerr;
        conn->Close();
    });
    io::WriteString(*t.conn, "GET / HTTP/1.1\r\nHost: e.com\r\n\r\n" + needle);
    ASSERT_FALSE(dynamic_cast<net::TCPConn&>(*t.conn).CloseWrite());
    done.recv();
}

// ---------------------------------------------------------------------------
// Request contexts (serve_test.go)

TEST_P(GoNetHttpClientServer, ServerRequestContextCancel_ServeHTTPDone) {
    auto ctxc = make_chan<ptr<context::Context>>(1);
    auto& ts = serve([ctxc](http::ResponseWriter&, http::Request& r) {
        auto ctx = r.Context();
        bool done = false;
        empty e;
        ctx->Done().tryRecv(e, &done);
        EXPECT_FALSE(done) << "should not be Done in ServeHTTP";
        ctxc.send(ctx);
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << err;
    res->Body->Close();
    auto ctx = ctxc.recv();
    bool done = false;
    empty e;
    ctx->Done().tryRecv(e, &done);
    EXPECT_TRUE(done) << "context should be done after ServeHTTP completes";
}

// The handler's context is canceled when the client closes its connection
// (Go issues 15927 and 15224): the server reads in the background.
TEST(GoNetHttpServe, ServerRequestContextCancel_ConnClose) {
    auto inHandler = make_chan<empty>();
    auto handlerDone = make_chan<empty>();
    rawConnTest t([inHandler, handlerDone](http::ResponseWriter&, http::Request& r) {
        inHandler.close();
        r.Context()->Done().recv();
        handlerDone.close();
    });
    io::WriteString(*t.conn, "GET / HTTP/1.1\r\nHost: foo\r\n\r\n");
    inHandler.recv();
    t.conn->Close();  // this cancels the context
    t.conn = nullptr;
    handlerDone.recv();
}

// Requests whose handlers ask for the context, pipelined: the background
// read takes the next request's first byte, which mustn't be lost.
TEST(GoNetHttpServe, ServerDuplicateBackgroundRead) {
    auto hts = httptest::NewServer(make_ptr<http::HandlerFunc>([](http::ResponseWriter& w, http::Request& r) {
        r.Context();
        http::NotFound(w, r);
    }));
    constexpr int fibers = 3, requests = 200;
    sync::WaitGroup wg;
    std::atomic<int> responses = 0;
    for (int i = 0; i < fibers; i++) {
        wg.Go([&] {
            auto [cn, err] = net::Dial("tcp", hts->Listener->Addr()->String());
            ASSERT_FALSE(err) << err;
            sync::WaitGroup readers;
            readers.Go([cn, &responses] {
                auto br = make_ptr<bufio::Reader>(cn);
                for (;;) {
                    auto [line, rerr] = br->ReadString('\n');
                    if (rerr) return;
                    if (line.starts_with("HTTP/1.1 404")) responses++;
                }
            });
            std::string reqs;
            for (int j = 0; j < requests; j++) reqs += "GET / HTTP/1.1\r\nHost: e.com\r\n\r\n";
            ASSERT_FALSE(io::WriteString(*cn, reqs).second);
            dynamic_cast<net::TCPConn&>(*cn).CloseWrite();
            readers.Wait();
            cn->Close();
        });
    }
    wg.Wait();
    EXPECT_EQ(responses.load(), fibers * requests);
    hts->Close();
}

// The Reader of a hijacked connection has the byte the background read
// took.
TEST(GoNetHttpServe, ServerHijackGetsBackgroundByte) {
    auto done = make_chan<empty>();
    auto inHandler = make_chan<bool>(1);
    rawConnTest t([done, inHandler](http::ResponseWriter& w, http::Request& r) {
        struct closer {
            chan<empty> c;
            ~closer() { c.close(); }
        } closeDone{done};
        auto ctx = r.Context();  // the server reads in the background now
        // Tell the client to send more after the request.
        inHandler.send(true);
        time::Sleep(20 * time::Millisecond);  // for the background read to take a byte
        auto [conn, buf, err] = dynamic_cast<http::Hijacker&>(w).Hijack();
        if (err) {
            ADD_FAILURE() << err;
            return;
        }
        auto [peek, perr] = buf->Reader->Peek(3);
        EXPECT_TRUE(asStringView(peek) == "foo" && !perr) << "Peek = " << std::string(asStringView(peek)) << ", " << perr;
        bool canceled = false;
        empty e;
        ctx->Done().tryRecv(e, &canceled);
        EXPECT_FALSE(canceled) << "context unexpectedly canceled";
        conn->Close();
    });
    io::WriteString(*t.conn, "GET / HTTP/1.1\r\nHost: e.com\r\n\r\n");
    inHandler.recv();
    io::WriteString(*t.conn, "foo");
    ASSERT_FALSE(dynamic_cast<net::TCPConn&>(*t.conn).CloseWrite());
    done.recv();
}
