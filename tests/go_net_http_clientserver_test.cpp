// Ported from Go 1.27.3's net/http/clientserver_test.go: the tests of
// trailers, run over HTTP/1.1, HTTP/1.1 over TLS and HTTP/2, with Go's test
// names.
//
// Adapted: Go's Trailer holds the announced keys with nil values before
// the body is read; go++'s Header can't hold a key without a value, so the
// Trailer is empty until then.
#include "net/http/internal.hpp"

#include <gopp/io.hpp>
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
