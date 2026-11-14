// Ported from Go 1.27.3's net/http/httputil/reverseproxy_test.go, with Go's
// test names. Left out: the tests of protocol upgrades (WebSocket) and
// hijacking, of trailers and 1xx responses, which go++'s HTTP doesn't
// carry, and of unexported functions. TestReverseProxy's hangup case
// proxies to a port that refuses connections instead of a hijacked connection.
#include <gopp/bytes.hpp>
#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/log.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/net/http/httputil.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

using namespace gopp;

namespace {

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

ptr<http::Handler> handlerFunc(http::handler_fn f) { return make_ptr<http::HandlerFunc>(std::move(f)); }

url::URL parse(std::string_view s) {
    auto [u, err] = url::Parse(s);
    EXPECT_FALSE(err) << err;
    return u;
}

ptr<log::Logger> quiet() {
    struct discard final : io::Writer {
        std::pair<isize, error> Write(std::span<const byte> p) override { return {isize(p.size()), nullptr}; }
    };
    return log::New(make_ptr<discard>(), "", 0);
}

std::string readBody(const ptr<http::Response>& res) {
    auto [b, err] = io::ReadAll(*res->Body);
    EXPECT_FALSE(err) << err;
    res->Body->Close();
    return str(b);
}

// A server, closed when it goes out of scope.
struct server {
    ptr<httptest::Server> ts;
    explicit server(ptr<http::Handler> h) : ts(httptest::NewServer(std::move(h))) {}
    ~server() { ts->Close(); }
    httptest::Server* operator->() const { return ts.get(); }
};

TEST(GoHttputil, ReverseProxy) {
    const std::string backendResponse = "I am the backend";
    constexpr int backendStatus = 404;
    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        EXPECT_TRUE(r.TransferEncoding.empty()) << "backend got unexpected TransferEncoding";
        EXPECT_NE(r.Header.Get("X-Forwarded-For"), "") << "didn't get X-Forwarded-For header";
        EXPECT_EQ(r.Header.Get("Connection"), "") << "handler got Connection header";
        EXPECT_EQ(r.Header.Get("Te"), "trailers") << "handler got Te header value";
        EXPECT_EQ(r.Header.Get("Upgrade"), "") << "handler got Upgrade header";
        EXPECT_EQ(r.Header.Get("Proxy-Connection"), "") << "handler got Proxy-Connection header";
        EXPECT_EQ(r.Host, "some-name") << "backend got Host header";
        w.Header().Set("Trailers", "not a special header field name");
        w.Header().Set("X-Foo", "bar");
        w.Header().Set("Upgrade", "foo");
        w.Header().Set("Keep-Alive", "foo");
        w.Header().Add("X-Multi-Value", "foo");
        w.Header().Add("X-Multi-Value", "bar");
        w.Header().Add("Set-Cookie", "flavor=chocolateChip");
        w.WriteHeader(backendStatus);
        io::WriteString(w, backendResponse);
    }));
    auto proxyHandler = httputil::NewSingleHostReverseProxy(parse(backend->URL));
    proxyHandler->ErrorLog = quiet();
    server frontend(proxyHandler);
    auto& frontendClient = frontend->Client();

    auto [getReq, _] = http::NewRequest("GET", frontend->URL);
    getReq->Host = "some-name";
    getReq->Header.Set("Connection", "close, TE");
    getReq->Header.Add("Te", "foo");
    getReq->Header.Add("Te", "bar, trailers");
    getReq->Header.Set("Proxy-Connection", "should be deleted");
    getReq->Header.Set("Upgrade", "foo");
    getReq->Close = true;
    auto [res, err] = frontendClient.Do(getReq);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(res->StatusCode, backendStatus);
    EXPECT_EQ(res->Header.Get("X-Foo"), "bar");
    EXPECT_EQ(res->Header.Get("Keep-Alive"), "") << "got a hop-by-hop header";
    EXPECT_EQ(res->Header.Get("Trailers"), "not a special header field name");
    EXPECT_EQ(res->Header.Values("X-Multi-Value").size(), 2u);
    EXPECT_EQ(res->Header.Values("Set-Cookie").size(), 1u);
    EXPECT_EQ(readBody(res), backendResponse);

    // Test that a backend failing to be reached or one which doesn't return
    // a response results in a StatusBadGateway.
    // (A port that refuses connections: a closed test server's port may
    // be the next server's.)
    std::string closedURL = "http://127.0.0.1:1";
    auto badProxy = httputil::NewSingleHostReverseProxy(parse(closedURL));
    badProxy->ErrorLog = quiet();
    server badFrontend(badProxy);
    std::tie(res, err) = badFrontend->Client().Get(badFrontend->URL + "/?mode=hangup");
    ASSERT_FALSE(err) << err;
    res->Body->Close();
    EXPECT_EQ(res->StatusCode, http::StatusBadGateway) << "request to bad proxy = " << res->Status;
}

// Issue 16875: remove any proxied headers mentioned in the "Connection"
// header value.
TEST(GoHttputil, ReverseProxyStripHeadersPresentInConnection) {
    const std::string fakeConnectionToken = "X-Fake-Connection-Token";
    const std::string backendResponse = "I am the backend";
    // someConnHeader is some arbitrary header to be declared as a hop-by-hop
    // header in the Request's Connection header.
    const std::string someConnHeader = "X-Some-Conn-Header";

    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(r.Header.Get("Connection"), "");
        EXPECT_EQ(r.Header.Get(fakeConnectionToken), "");
        EXPECT_EQ(r.Header.Get(someConnHeader), "");
        w.Header().Add("Connection", string("Upgrade, " + fakeConnectionToken));
        w.Header().Add("Connection", string(someConnHeader));
        w.Header().Set(someConnHeader, "should be deleted");
        w.Header().Set(fakeConnectionToken, "should be deleted");
        io::WriteString(w, backendResponse);
    }));
    auto proxyHandler = httputil::NewSingleHostReverseProxy(parse(backend->URL));
    server frontend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        proxyHandler->ServeHTTP(w, r);
        EXPECT_EQ(r.Header.Get(someConnHeader), "should be deleted") << "handler modified header";
        EXPECT_EQ(r.Header.Get(fakeConnectionToken), "should be deleted") << "handler modified header";
        std::vector<std::string> cf;
        for (const auto& f : r.Header.Values("Connection")) {
            for (auto sf : strings::Split(f, ",")) {
                if (sf = strings::TrimSpace(sf); !sf.empty()) cf.push_back(sf.str());
            }
        }
        std::sort(cf.begin(), cf.end());
        std::vector<std::string> expectedValues = {"Upgrade", someConnHeader, fakeConnectionToken};
        std::sort(expectedValues.begin(), expectedValues.end());
        EXPECT_EQ(cf, expectedValues) << "handler modified header Connection";
    }));

    auto [getReq, _] = http::NewRequest("GET", frontend->URL);
    getReq->Header.Add("Connection", string("Upgrade, " + fakeConnectionToken));
    getReq->Header.Add("Connection", string(someConnHeader));
    getReq->Header.Set(someConnHeader, "should be deleted");
    getReq->Header.Set(fakeConnectionToken, "should be deleted");
    auto [res, err] = frontend->Client().Do(getReq);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(readBody(res), backendResponse);
    EXPECT_EQ(res->Header.Get("Connection"), "");
    EXPECT_EQ(res->Header.Get(someConnHeader), "");
    EXPECT_EQ(res->Header.Get(fakeConnectionToken), "");
}

TEST(GoHttputil, ReverseProxyStripEmptyConnection) {
    // See Issue 46313.
    const std::string backendResponse = "I am the backend";
    const std::string someConnHeader = "X-Some-Conn-Header";
    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        EXPECT_TRUE(r.Header.Values("Connection").empty()) << "handler got header Connection";
        EXPECT_EQ(r.Header.Get(someConnHeader), "");
        w.Header().Add("Connection", "");
        w.Header().Add("Connection", string(someConnHeader));
        w.Header().Set(someConnHeader, "should be deleted");
        io::WriteString(w, backendResponse);
    }));
    auto proxyHandler = httputil::NewSingleHostReverseProxy(parse(backend->URL));
    server frontend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        proxyHandler->ServeHTTP(w, r);
        EXPECT_EQ(r.Header.Get(someConnHeader), "should be deleted") << "handler modified header";
    }));
    auto [getReq, _] = http::NewRequest("GET", frontend->URL);
    getReq->Header.Add("Connection", "");
    getReq->Header.Add("Connection", string(someConnHeader));
    getReq->Header.Set(someConnHeader, "should be deleted");
    auto [res, err] = frontend->Client().Do(getReq);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(readBody(res), backendResponse);
    EXPECT_EQ(res->Header.Get("Connection"), "");
    EXPECT_EQ(res->Header.Get(someConnHeader), "");
}

TEST(GoHttputil, XForwardedFor) {
    const std::string prevForwardedFor = "client ip";
    const std::string backendResponse = "I am the backend";
    constexpr int backendStatus = 404;
    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        EXPECT_NE(r.Header.Get("X-Forwarded-For"), "") << "didn't get X-Forwarded-For header";
        EXPECT_NE(r.Header.Get("X-Forwarded-For").view().find(prevForwardedFor), std::string_view::npos)
            << "X-Forwarded-For didn't contain prior data";
        w.WriteHeader(backendStatus);
        io::WriteString(w, backendResponse);
    }));
    server frontend(httputil::NewSingleHostReverseProxy(parse(backend->URL)));
    auto [getReq, _] = http::NewRequest("GET", frontend->URL);
    getReq->Header.Set("Connection", "close");
    getReq->Header.Set("X-Forwarded-For", string(prevForwardedFor));
    getReq->Close = true;
    auto [res, err] = frontend->Client().Do(getReq);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(res->StatusCode, backendStatus);
    EXPECT_EQ(readBody(res), backendResponse);
}

TEST(GoHttputil, ReverseProxyRewriteStripsForwarded) {
    std::vector<std::string> headers = {"Forwarded", "X-Forwarded-For", "X-Forwarded-Host", "X-Forwarded-Proto"};
    server backend(handlerFunc([&](http::ResponseWriter&, http::Request& r) {
        for (const auto& h : headers) EXPECT_EQ(r.Header.Get(h), "") << "got " << h << " header";
    }));
    auto backendURL = parse(backend->URL);
    auto proxyHandler = make_ptr<httputil::ReverseProxy>();
    proxyHandler->Rewrite = [backendURL](httputil::ProxyRequest& r) { r.SetURL(backendURL); };
    server frontend(proxyHandler);
    auto [getReq, _] = http::NewRequest("GET", frontend->URL);
    getReq->Host = "some-name";
    getReq->Close = true;
    for (const auto& h : headers) getReq->Header.Set(h, "x");
    auto [res, err] = frontend->Client().Do(getReq);
    ASSERT_FALSE(err) << "Get: " << err;
    res->Body->Close();
}

TEST(GoHttputil, ReverseProxyQuery) {
    struct test {
        std::string baseSuffix;  // suffix to add to backend URL
        std::string reqSuffix;   // suffix to add to frontend's request URL
        std::string want;        // what backend should see for final request URL (without ?)
    };
    server backend(handlerFunc([](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("X-Got-Query", r.URL.RawQuery);
        io::WriteString(w, "hi");
    }));
    int i = 0;
    for (const auto& tt : std::vector<test>{
             {"", "", ""}, {"?sta=tic", "?us=er", "sta=tic&us=er"}, {"", "?us=er", "us=er"}, {"?sta=tic", "", "sta=tic"}}) {
        server frontend(httputil::NewSingleHostReverseProxy(parse(std::string(backend->URL.view()) + tt.baseSuffix)));
        auto [req, _] = http::NewRequest("GET", std::string(frontend->URL.view()) + tt.reqSuffix);
        req->Close = true;
        auto [res, err] = frontend->Client().Do(req);
        ASSERT_FALSE(err) << i << ". Get: " << err;
        EXPECT_EQ(res->Header.Get("X-Got-Query"), tt.want) << i;
        res->Body->Close();
        i++;
    }
}

TEST(GoHttputil, ReverseProxyFlushInterval) {
    const std::string expected = "hi";
    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request&) { io::WriteString(w, expected); }));
    auto proxyHandler = httputil::NewSingleHostReverseProxy(parse(backend->URL));
    proxyHandler->FlushInterval = time::Microsecond;
    server frontend(proxyHandler);
    auto [req, _] = http::NewRequest("GET", frontend->URL);
    req->Close = true;
    auto [res, err] = frontend->Client().Do(req);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(readBody(res), expected);
}

// A streaming body of unknown length is flushed after every write: the
// client sees the first part before the backend sends the rest. (Go tests
// selectFlushInterval, unexported.)
TEST(GoHttputil, ReverseProxyFlushIntervalStreaming) {
    auto next = makeChan<empty>();
    server backend(handlerFunc([next](http::ResponseWriter& w, http::Request&) {
        io::WriteString(w, "first ");
        dynamic_cast<http::Flusher&>(w).Flush();
        next.recv();
        io::WriteString(w, "second");
    }));
    server frontend(httputil::NewSingleHostReverseProxy(parse(backend->URL)));
    auto [res, err] = frontend->Client().Get(frontend->URL);
    ASSERT_FALSE(err) << err;
    byte buf[6];
    auto [n, rerr] = io::ReadFull(*res->Body, buf);
    ASSERT_FALSE(rerr);
    EXPECT_EQ(str(buf), "first ");
    next.send({});
    EXPECT_EQ(readBody(res), "second");
}

TEST(GoHttputil, UserAgentHeader) {
    auto gotUA = std::make_shared<std::string>();
    server backend(handlerFunc([gotUA](http::ResponseWriter&, http::Request& r) { *gotUA = r.Header.Get("User-Agent").str(); }));
    auto backendURL = parse(backend->URL);
    auto proxyHandler = make_ptr<httputil::ReverseProxy>();
    proxyHandler->ErrorLog = quiet();
    proxyHandler->Director = [backendURL](http::Request& req) { req.URL = backendURL; };
    server frontend(proxyHandler);
    for (std::string sentUA : {"explicit UA", ""}) {
        auto [getReq, _] = http::NewRequest("GET", frontend->URL);
        getReq->Header.Set("User-Agent", string(sentUA));
        getReq->Close = true;
        auto [res, err] = frontend->Client().Do(getReq);
        ASSERT_FALSE(err) << "Get: " << err;
        res->Body->Close();
        EXPECT_EQ(*gotUA, sentUA) << "got forwarded User-Agent";
    }
}

struct bufferPool final : httputil::BufferPool {
    std::function<slice<byte>()> get;
    std::function<void(slice<byte>)> put;
    slice<byte> Get() override { return get(); }
    void Put(slice<byte> v) override { put(std::move(v)); }
};

TEST(GoHttputil, ReverseProxyGetPutBuffer) {
    const std::string msg = "hi";
    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request&) { io::WriteString(w, msg); }));
    sync::Mutex mu;
    std::vector<std::string> log;
    auto addLog = [&](std::string event) {
        std::lock_guard l(mu);
        log.push_back(std::move(event));
    };
    auto rp = httputil::NewSingleHostReverseProxy(parse(backend->URL));
    constexpr std::size_t size = 1234;
    auto bp = make_ptr<bufferPool>();
    bp->get = [&] {
        addLog("getBuf");
        return slice<byte>::make(size);
    };
    bp->put = [&](slice<byte> p) { addLog("putBuf-" + std::to_string(p.len())); };
    rp->BufferPool = bp;
    server frontend(rp);
    auto [req, _] = http::NewRequest("GET", frontend->URL);
    req->Close = true;
    auto [res, err] = frontend->Client().Do(req);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(readBody(res), msg);
    std::lock_guard l(mu);
    EXPECT_EQ(log, (std::vector<std::string>{"getBuf", "putBuf-" + std::to_string(size)}));
}

TEST(GoHttputil, ReverseProxy_Post) {
    const std::string backendResponse = "I am the backend";
    std::string requestBody(1 << 20, 'a');
    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        auto [slurp, err] = io::ReadAll(*r.Body);
        EXPECT_FALSE(err) << "Backend body read = " << err;
        EXPECT_EQ(slurp.len(), requestBody.size());
        EXPECT_TRUE(str(slurp) == requestBody) << "Backend read wrong request body.";
        io::WriteString(w, backendResponse);
    }));
    server frontend(httputil::NewSingleHostReverseProxy(parse(backend->URL)));
    auto [postReq, _] = http::NewRequest("POST", frontend->URL, bytes::NewReader(toBytes(requestBody)));
    auto [res, err] = frontend->Client().Do(postReq);
    ASSERT_FALSE(err) << "Do: " << err;
    EXPECT_EQ(res->StatusCode, 200);
    EXPECT_EQ(readBody(res), backendResponse);
}

struct roundTripperFunc final : http::RoundTripper {
    std::function<std::pair<ptr<http::Response>, error>(http::Request&)> fn;
    explicit roundTripperFunc(decltype(fn) f) : fn(std::move(f)) {}
    std::pair<ptr<http::Response>, error> RoundTrip(http::Request& req) override { return fn(req); }
};

// Issue 16036: send a Request with a nil Body when possible.
TEST(GoHttputil, ReverseProxy_NilBody) {
    auto proxyHandler = httputil::NewSingleHostReverseProxy(parse("http://fake.tld/"));
    proxyHandler->ErrorLog = quiet();
    proxyHandler->Transport = make_ptr<roundTripperFunc>([](http::Request& req) {
        EXPECT_FALSE(req.Body) << "Body != nil; want a nil Body";
        return std::pair<ptr<http::Response>, error>{
            nullptr, errors::New("done testing the interesting part; so force a 502 Gateway error")};
    });
    server frontend(proxyHandler);
    auto [res, err] = frontend->Client().Get(frontend->URL);
    ASSERT_FALSE(err) << err;
    res->Body->Close();
    EXPECT_EQ(res->StatusCode, 502) << "status code = " << res->Status << "; want 502 (Gateway Error)";
}

TEST(GoHttputil, ReverseProxyModifyResponse) {
    server backendServer(handlerFunc([](http::ResponseWriter& w, http::Request& r) {
        w.Header().Add("X-Hit-Mod", string::fromStatic(r.URL.Path == "/mod" ? "true" : "false"));
    }));
    auto rproxy = httputil::NewSingleHostReverseProxy(parse(backendServer->URL));
    rproxy->ErrorLog = quiet();
    rproxy->ModifyResponse = [](http::Response& resp) -> error {
        if (resp.Header.Get("X-Hit-Mod") != "true") return errors::New("tried to by-pass proxy");
        return nullptr;
    };
    server frontendProxy(rproxy);
    struct test {
        std::string url;
        int wantCode;
    };
    int i = 0;
    for (const auto& tt : std::vector<test>{{std::string(frontendProxy->URL.view()) + "/mod", http::StatusOK},
                                            {std::string(frontendProxy->URL.view()) + "/schedule", http::StatusBadGateway}}) {
        auto [resp, err] = frontendProxy->Client().Get(tt.url);
        ASSERT_FALSE(err) << "failed to reach proxy: " << err;
        EXPECT_EQ(resp->StatusCode, tt.wantCode) << "#" << i;
        resp->Body->Close();
        i++;
    }
}

TEST(GoHttputil, ReverseProxyErrorHandler) {
    using errorHandlerFn = std::function<void(http::ResponseWriter&, http::Request&, error)>;
    auto staticResponse = [](int code) {
        return make_ptr<roundTripperFunc>([code](http::Request&) {
            auto res = make_ptr<http::Response>();
            res->StatusCode = code;
            res->Body = io::NopCloser(strings::NewReader(""));
            return std::pair<ptr<http::Response>, error>{res, nullptr};
        });
    };
    auto teapot = [](http::ResponseWriter& rw, http::Request&, error) { rw.WriteHeader(http::StatusTeapot); };
    struct test {
        std::string name;
        int wantCode;
        errorHandlerFn errorHandler;
        ptr<http::RoundTripper> transport;  // defaults to failingRoundTripper
        std::function<error(http::Response&)> modifyResponse;
    };
    std::vector<test> tests = {
        {"default", http::StatusBadGateway, nullptr, nullptr, nullptr},
        {"errorhandler", http::StatusTeapot, teapot, nullptr, nullptr},
        {"modifyresponse_noerr", 346, teapot, staticResponse(345),
         [](http::Response& res) -> error {
             res.StatusCode++;
             return nullptr;
         }},
        {"modifyresponse_err", http::StatusTeapot, teapot, staticResponse(345),
         [](http::Response& res) -> error {
             res.StatusCode++;
             return errors::New("some error to trigger errorHandler");
         }},
    };
    for (const auto& tt : tests) {
        SCOPED_TRACE(tt.name);
        auto rproxy = httputil::NewSingleHostReverseProxy(parse("http://dummy.tld/"));
        rproxy->Transport = tt.transport;
        rproxy->ModifyResponse = tt.modifyResponse;
        if (!rproxy->Transport) {
            rproxy->Transport = make_ptr<roundTripperFunc>([](http::Request&) {
                return std::pair<ptr<http::Response>, error>{nullptr, errors::New("some error")};
            });
        }
        rproxy->ErrorLog = quiet();
        if (tt.errorHandler) rproxy->ErrorHandler = tt.errorHandler;
        server frontendProxy(rproxy);
        auto [resp, err] = frontendProxy->Client().Get(frontendProxy->URL + "/test");
        ASSERT_FALSE(err) << "failed to reach proxy: " << err;
        EXPECT_EQ(resp->StatusCode, tt.wantCode);
        resp->Body->Close();
    }
}

// Issue 18327: verify we always do a deep copy of the Request.Header map
// before any mutations.
TEST(GoHttputil, ServeHTTPDeepCopy) {
    server backend(handlerFunc([](http::ResponseWriter& w, http::Request&) { io::WriteString(w, "Hello Gopher!"); }));
    auto resultChan = makeChan<std::pair<std::string, std::string>>(1);
    auto proxyHandler = httputil::NewSingleHostReverseProxy(parse(backend->URL));
    server frontend(handlerFunc([&](http::ResponseWriter& w, http::Request& r) {
        std::string before = r.URL.String().str();
        proxyHandler->ServeHTTP(w, r);
        std::string after = r.URL.String().str();
        resultChan.send({before, after});
    }));
    auto [res, err] = frontend->Client().Get(frontend->URL);
    ASSERT_FALSE(err) << "Do: " << err;
    res->Body->Close();
    auto got = resultChan.recv();
    EXPECT_EQ(got, (std::pair<std::string, std::string>{"/", "/"}));
}

// Issue 18327: verify we always do a deep copy of the Request.Header map
// before any mutations.
TEST(GoHttputil, ClonesRequestHeaders) {
    auto [req, _] = http::NewRequest("GET", "http://foo.tld/");
    req->RemoteAddr = "1.2.3.4:56789";
    auto rp = make_ptr<httputil::ReverseProxy>();
    rp->ErrorLog = quiet();
    rp->Director = [](http::Request& req) { req.Header.Set("From-Director", "1"); };
    rp->Transport = make_ptr<roundTripperFunc>([](http::Request& req) {
        EXPECT_EQ(req.Header.Get("From-Director"), "1");
        return std::pair<ptr<http::Response>, error>{nullptr, io::EOF_};
    });
    rp->ServeHTTP(*httptest::NewRecorder(), *req);
    for (std::string h : {"From-Director", "X-Forwarded-For"}) {
        EXPECT_EQ(req->Header.Get(h), "") << h << " header mutation modified caller's request";
    }
}

struct checkCloser final : io::ReadCloser {
    bool closed = false;
    error Close() override {
        closed = true;
        return nullptr;
    }
    std::pair<isize, error> Read(std::span<byte> b) override { return {isize(b.size()), nullptr}; }
};

TEST(GoHttputil, ModifyResponseClosesBody) {
    auto [req, _] = http::NewRequest("GET", "http://foo.tld/");
    req->RemoteAddr = "1.2.3.4:56789";
    auto closeCheck = make_ptr<checkCloser>();
    auto logBuf = make_ptr<strings::Builder>();
    auto outErr = errors::New("ModifyResponse error");
    auto rp = make_ptr<httputil::ReverseProxy>();
    rp->Director = [](http::Request&) {};
    rp->Transport = make_ptr<roundTripperFunc>([closeCheck](http::Request&) {
        auto res = make_ptr<http::Response>();
        res->StatusCode = 200;
        res->Body = closeCheck;
        return std::pair<ptr<http::Response>, error>{res, nullptr};
    });
    rp->ErrorLog = log::New(logBuf, "", 0);
    rp->ModifyResponse = [outErr](http::Response&) { return outErr; };
    auto rec = httptest::NewRecorder();
    rp->ServeHTTP(*rec, *req);
    EXPECT_EQ(rec->Result()->StatusCode, http::StatusBadGateway);
    EXPECT_TRUE(closeCheck->closed) << "body should have been closed";
    EXPECT_NE(logBuf->String().view().find(outErr.Error().view()), std::string_view::npos)
        << "ErrorLog " << logBuf->String() << " does not contain " << outErr;
}

TEST(GoHttputil, ReverseProxy_PanicBodyError) {
    server backendServer(handlerFunc([](http::ResponseWriter& w, http::Request&) {
        std::string out = "this call was relayed by the reverse proxy";
        // Coerce a wrong content length to induce io.ErrUnexpectedEOF.
        w.Header().Set("Content-Length", strconv::Itoa(int64(out.size() * 2)));
        fmt::Println(w, "{}", out);
    }));
    auto rproxy = httputil::NewSingleHostReverseProxy(parse(backendServer->URL));
    rproxy->ErrorLog = quiet();
    // Ensure that the handler panics (throws) when the body read encounters
    // an io.ErrUnexpectedEOF.
    auto [req, _] = http::NewRequest("GET", "http://foo.tld/");
    try {
        rproxy->ServeHTTP(*httptest::NewRecorder(), *req);
        FAIL() << "handler should have panicked";
    } catch (const error& err) {
        EXPECT_EQ(err, http::ErrAbortHandler) << "expected ErrAbortHandler, got " << err;
    }
}

// Issue #46866: panic without closing incoming request body causes a panic.
TEST(GoHttputil, ReverseProxy_PanicClosesIncomingBody) {
    server backend(handlerFunc([](http::ResponseWriter& w, http::Request&) {
        std::string out = "this call was relayed by the reverse proxy";
        // Coerce a wrong content length to induce io.ErrUnexpectedEOF.
        w.Header().Set("Content-Length", strconv::Itoa(int64(out.size() * 2)));
        fmt::Println(w, "{}", out);
    }));
    auto proxyHandler = httputil::NewSingleHostReverseProxy(parse(backend->URL));
    proxyHandler->ErrorLog = quiet();
    server frontend(proxyHandler);
    auto transport = frontend->Client().Transport;
    struct neverEnding final : io::Reader {
        std::pair<isize, error> Read(std::span<byte> p) override {
            for (auto& c : p) c = 'x';
            return {isize(p.size()), nullptr};
        }
    };
    sync::WaitGroup wg;
    string url = frontend->URL;
    for (int i = 0; i < 2; i++) {
        wg.Go([transport, url] {
            for (int j = 0; j < 10; j++) {
                constexpr int64 reqLen = 6 * 1024 * 1024;
                auto [req, _] = http::NewRequest("POST", url, make_ptr<io::LimitedReader>(make_ptr<neverEnding>(), reqLen));
                req->ContentLength = reqLen;
                auto [resp, _2] = transport->RoundTrip(*req);
                if (resp) {
                    io::Copy(io::Discard(), *resp->Body);
                    resp->Body->Close();
                }
            }
        });
    }
    wg.Wait();
}

TEST(GoHttputil, SetURL) {
    server backend(handlerFunc([](http::ResponseWriter& w, http::Request& r) { io::WriteString(w, r.Host); }));
    auto backendURL = parse(backend->URL);
    auto proxyHandler = make_ptr<httputil::ReverseProxy>();
    proxyHandler->Rewrite = [backendURL](httputil::ProxyRequest& r) { r.SetURL(backendURL); };
    server frontend(proxyHandler);
    auto [res, err] = frontend->Client().Get(frontend->URL);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(readBody(res), backendURL.Host) << "backend got Host";
}

TEST(GoHttputil, ReverseProxyRewriteReplacesOut) {
    const std::string content = "response_content";
    server backend(handlerFunc([&](http::ResponseWriter& w, http::Request&) { io::WriteString(w, content); }));
    string backendURL = backend->URL;
    auto proxyHandler = make_ptr<httputil::ReverseProxy>();
    proxyHandler->Rewrite = [backendURL](httputil::ProxyRequest& r) { r.Out = http::NewRequest("GET", backendURL).first; };
    server frontend(proxyHandler);
    auto [res, err] = frontend->Client().Get(frontend->URL);
    ASSERT_FALSE(err) << "Get: " << err;
    EXPECT_EQ(readBody(res), content);
}

void testReverseProxyQueryParameterSmuggling(bool wantCleanQuery,
                                             std::function<ptr<httputil::ReverseProxy>(const url::URL&)> newProxy) {
    server backend(handlerFunc([](http::ResponseWriter& w, http::Request& r) { io::WriteString(w, r.URL.RawQuery); }));
    server frontend(newProxy(parse(backend->URL)));
    struct test {
        std::string rawQuery, cleanQuery;
    };
    // Go also tests 10001 parameters, which url.ParseQuery refuses; go++'s
    // ParseQuery has no such limit.
    for (const auto& tt : std::vector<test>{{"a=1&a=2;b=3", "a=1"}, {"a=1&a=%zz&b=3", "a=1&b=3"}, {"a=%zz", ""}}) {
        auto [res, err] = frontend->Client().Get(std::string(frontend->URL.view()) + "?" + tt.rawQuery);
        ASSERT_FALSE(err) << "Get: " << err;
        std::string want = wantCleanQuery ? tt.cleanQuery : tt.rawQuery;
        EXPECT_EQ(readBody(res), want) << "proxy forwarded raw query " << tt.rawQuery;
    }
}

TEST(GoHttputil, ReverseProxyQueryParameterSmugglingDirectorDoesNotParseForm) {
    testReverseProxyQueryParameterSmuggling(false, [](const url::URL& u) {
        auto proxyHandler = httputil::NewSingleHostReverseProxy(u);
        auto oldDirector = proxyHandler->Director;
        proxyHandler->Director = [oldDirector](http::Request& r) { oldDirector(r); };
        return proxyHandler;
    });
}

TEST(GoHttputil, ReverseProxyQueryParameterSmugglingDirectorParsesForm) {
    testReverseProxyQueryParameterSmuggling(true, [](const url::URL& u) {
        auto proxyHandler = httputil::NewSingleHostReverseProxy(u);
        auto oldDirector = proxyHandler->Director;
        proxyHandler->Director = [oldDirector](http::Request& r) {
            // Parsing the form causes ReverseProxy to remove unparsable
            // query parameters before forwarding.
            r.FormValue("a");
            oldDirector(r);
        };
        return proxyHandler;
    });
}

TEST(GoHttputil, ReverseProxyQueryParameterSmugglingRewrite) {
    testReverseProxyQueryParameterSmuggling(true, [](const url::URL& u) {
        auto p = make_ptr<httputil::ReverseProxy>();
        p->Rewrite = [u](httputil::ProxyRequest& r) { r.SetURL(u); };
        return p;
    });
}

TEST(GoHttputil, ReverseProxyQueryParameterSmugglingRewritePreservesRawQuery) {
    testReverseProxyQueryParameterSmuggling(false, [](const url::URL& u) {
        auto p = make_ptr<httputil::ReverseProxy>();
        p->Rewrite = [u](httputil::ProxyRequest& r) {
            r.SetURL(u);
            r.Out->URL.RawQuery = r.In->URL.RawQuery;
        };
        return p;
    });
}

TEST(GoHttputil, ReverseProxyCONNECT) {
    auto proxy = make_ptr<httputil::ReverseProxy>();
    proxy->ErrorLog = quiet();
    proxy->Rewrite = [](httputil::ProxyRequest& r) { r.SetURL(parse("http://backend.tld/")); };
    server frontend(proxy);
    auto [req, _] = http::NewRequest("CONNECT", frontend->URL, strings::NewReader("body"));
    auto [resp, err] = frontend->Client().Do(req);
    ASSERT_FALSE(err) << err;
    resp->Body->Close();
    EXPECT_EQ(resp->StatusCode, http::StatusMethodNotAllowed) << "on response to CONNECT";
}

// go++: SetXForwarded sets the three headers.
TEST(Httputil, SetXForwarded) {
    server backend(handlerFunc([](http::ResponseWriter& w, http::Request& r) {
        fmt::Print(w, "{}|{}|{}", r.Header.Get("X-Forwarded-For"), r.Header.Get("X-Forwarded-Host"),
                   r.Header.Get("X-Forwarded-Proto"));
    }));
    auto backendURL = parse(backend->URL);
    auto proxy = make_ptr<httputil::ReverseProxy>();
    proxy->Rewrite = [backendURL](httputil::ProxyRequest& r) {
        r.SetURL(backendURL);
        r.SetXForwarded();
    };
    server frontend(proxy);
    auto [req, _] = http::NewRequest("GET", frontend->URL);
    req->Host = "example.com";
    auto [res, err] = frontend->Client().Do(req);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(readBody(res), "127.0.0.1|example.com|http");
}

}  // namespace
