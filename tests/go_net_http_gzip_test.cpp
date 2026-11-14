// Ported from Go 1.27.3's net/http/transport_test.go: the tests of the
// Transport's transparent gzip, in Go's modes (HTTP/1.1, HTTP/1.1 over TLS,
// HTTP/2), with Go's test names. go++ also tests DisableCompression and
// that the caller's request is left alone.
#include <gopp/bytes.hpp>
#include <gopp/chan.hpp>
#include <gopp/compress/gzip.hpp>
#include <gopp/crypto/rand.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace gopp;

// Go's test modes. (Not in the anonymous namespace: googletest's
// parameterized tests hold one.)
enum class httpGzipTestMode { http1, https1, http2 };

namespace {

using testMode = httpGzipTestMode;

std::string modeName(const testing::TestParamInfo<testMode>& info) {
    switch (info.param) {
    case testMode::http1: return "h1";
    case testMode::https1: return "https1";
    case testMode::http2: return "h2";
    }
    return "";
}

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

// A recursive gzip file: it decompresses to itself.
const std::vector<byte> rgz = {
    0x1f, 0x8b, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x65, 0x63, 0x75, 0x72, 0x73, 0x69, 0x76, 0x65,
    0x00, 0x92, 0xef, 0xe6, 0xe0, 0x60, 0x00, 0x83, 0xa2, 0xd4, 0xe4, 0xd2, 0xa2, 0xe2, 0xcc, 0xb2, 0x54, 0x06, 0x00,
    0x00, 0x17, 0x00, 0xe8, 0xff, 0x92, 0xef, 0xe6, 0xe0, 0x60, 0x00, 0x83, 0xa2, 0xd4, 0xe4, 0xd2, 0xa2, 0xe2, 0xcc,
    0xb2, 0x54, 0x06, 0x00, 0x00, 0x17, 0x00, 0xe8, 0xff, 0x42, 0x12, 0x46, 0x16, 0x06, 0x00, 0x05, 0x00, 0xfa, 0xff,
    0x42, 0x12, 0x46, 0x16, 0x06, 0x00, 0x05, 0x00, 0xfa, 0xff, 0x00, 0x05, 0x00, 0xfa, 0xff, 0x00, 0x14, 0x00, 0xeb,
    0xff, 0x42, 0x12, 0x46, 0x16, 0x06, 0x00, 0x05, 0x00, 0xfa, 0xff, 0x00, 0x05, 0x00, 0xfa, 0xff, 0x00, 0x14, 0x00,
    0xeb, 0xff, 0x42, 0x88, 0x21, 0xc4, 0x00, 0x00, 0x14, 0x00, 0xeb, 0xff, 0x42, 0x88, 0x21, 0xc4, 0x00, 0x00, 0x14,
    0x00, 0xeb, 0xff, 0x42, 0x88, 0x21, 0xc4, 0x00, 0x00, 0x14, 0x00, 0xeb, 0xff, 0x42, 0x88, 0x21, 0xc4, 0x00, 0x00,
    0x14, 0x00, 0xeb, 0xff, 0x42, 0x88, 0x21, 0xc4, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff,
    0x00, 0x17, 0x00, 0xe8, 0xff, 0x42, 0x88, 0x21, 0xc4, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff,
    0xff, 0x00, 0x17, 0x00, 0xe8, 0xff, 0x42, 0x12, 0x46, 0x16, 0x06, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01, 0x08, 0x00,
    0xf7, 0xff, 0x3d, 0xb1, 0x20, 0x85, 0xfa, 0x00, 0x00, 0x00, 0x42, 0x12, 0x46, 0x16, 0x06, 0x00, 0x00, 0x00, 0xff,
    0xff, 0x01, 0x08, 0x00, 0xf7, 0xff, 0x3d, 0xb1, 0x20, 0x85, 0xfa, 0x00, 0x00, 0x00, 0x3d, 0xb1, 0x20, 0x85, 0xfa,
    0x00, 0x00, 0x00,
};

class GoNetHttpGzip : public testing::TestWithParam<testMode> {
protected:
    // newClientServerTest: a test server in the mode, closed at the end.
    httptest::Server& serve(http::handlerFn f) {
        auto h = make_ptr<http::HandlerFunc>(std::move(f));
        ptr<httptest::Server> ts;
        http::Protocols p;
        switch (GetParam()) {
        case testMode::http1:
            ts = httptest::NewServer(std::move(h));
            break;
        case testMode::https1:
            p.SetHTTP1(true);
            ts = httptest::NewTLSServer(std::move(h), p);
            break;
        case testMode::http2:
            p.SetHTTP2(true);
            ts = httptest::NewTLSServer(std::move(h), p);
            break;
        }
        ts_ = ts;
        return *ts;
    }

    void TearDown() override {
        if (ts_) ts_->Close();
    }

    ptr<httptest::Server> ts_;
};

INSTANTIATE_TEST_SUITE_P(Modes, GoNetHttpGzip, testing::Values(testMode::http1, testMode::https1, testMode::http2),
                         modeName);

TEST_P(GoNetHttpGzip, TransportGzip) {
    const std::string testString = "The test string aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    constexpr int64 nRandBytes = 1024 * 1024;
    auto& ts = serve([testString](http::ResponseWriter& rw, http::Request& req) {
        if (req.Method == "HEAD") {
            EXPECT_EQ(req.Header.Get("Accept-Encoding"), "") << "HEAD request sent with Accept-Encoding";
            return;
        }
        EXPECT_EQ(req.Header.Get("Accept-Encoding"), "gzip");
        rw.Header().Set("Content-Encoding", "gzip");
        auto buf = make_ptr<bytes::Buffer>();
        bool chunked = req.FormValue("chunked") != "0";
        struct rwWriter final : io::Writer {
            http::ResponseWriter& w;
            explicit rwWriter(http::ResponseWriter& w_) : w(w_) {}
            std::pair<isize, error> Write(std::span<const byte> p) override { return w.Write(p); }
        };
        ptr<io::Writer> w = chunked ? ptr<io::Writer>(make_ptr<rwWriter>(rw)) : ptr<io::Writer>(buf);
        auto gz = gzip::NewWriter(w);
        io::WriteString(*gz, testString);
        if (req.FormValue("body") == "large") io::CopyN(*gz, rand::Reader, nRandBytes);
        gz->Close();
        if (!chunked) {
            rw.Header().Set("Content-Length", strconv::Itoa(buf->Len()));
            io::Copy(rw, *buf);
        }
    });
    auto& c = ts.Client();

    for (string chunked : {"1", "0"}) {
        // First fetch something large, but only read some of it.
        auto [res, err] = c.Get(ts.URL + "/?body=large&chunked=" + chunked);
        ASSERT_FALSE(err) << "large get: " << err;
        std::vector<byte> buf(testString.size());
        auto [n, rerr] = io::ReadFull(*res->Body, buf);
        ASSERT_FALSE(rerr) << "partial read of large response: size=" << n << ", " << rerr;
        EXPECT_EQ(str(buf), testString);
        res->Body->Close();
        // Read on the body, even though it's closed.
        auto [n2, err2] = res->Body->Read(buf);
        EXPECT_TRUE(n2 == 0 && err2) << "expected error post-closed large Read";

        // Then something small.
        std::tie(res, err) = c.Get(ts.URL + "/?chunked=" + chunked);
        ASSERT_FALSE(err) << err;
        auto [body, berr] = io::ReadAll(*res->Body);
        ASSERT_FALSE(berr) << berr;
        ASSERT_EQ(str(body), testString);
        ASSERT_EQ(res->Header.Get("Content-Encoding"), "");
        EXPECT_TRUE(res->Uncompressed);
        EXPECT_EQ(res->ContentLength, -1);

        // Read on the body after it's been fully read.
        auto [n3, err3] = res->Body->Read(buf);
        EXPECT_TRUE(n3 == 0 && err3) << "expected Read error after exhausted reads";
        res->Body->Close();
        auto [n4, err4] = res->Body->Read(buf);
        EXPECT_TRUE(n4 == 0 && err4) << "expected Read error after Close";
    }

    // And a HEAD request too, because they're always weird.
    auto [res, err] = c.Head(ts.URL);
    ASSERT_FALSE(err) << "Head: " << err;
    EXPECT_EQ(res->StatusCode, 200);
    res->Body->Close();
}

TEST_P(GoNetHttpGzip, TransportGzipRecursive) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Content-Encoding", "gzip");
        w.Write(rgz);
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << err;
    auto [body, berr] = io::ReadAll(*res->Body);
    ASSERT_FALSE(berr) << berr;
    res->Body->Close();
    ASSERT_EQ(std::vector<byte>(body.begin(), body.end()), rgz) << "Incorrect result from recursive gz";
    ASSERT_EQ(res->Header.Get("Content-Encoding"), "");
}

// golang.org/issue/7750: request fails when server replies with a short
// gzip body.
TEST_P(GoNetHttpGzip, TransportGzipShort) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Content-Encoding", "gzip");
        byte b[] = {0x1f, 0x8b};
        w.Write(b);
    });
    auto [res, err] = ts.Client().Get(ts.URL);
    ASSERT_FALSE(err) << err;
    auto [_, berr] = io::ReadAll(*res->Body);
    res->Body->Close();
    ASSERT_TRUE(berr) << "Expect an error from reading a body.";
    EXPECT_EQ(berr, io::ErrUnexpectedEOF) << "ReadAll error = " << berr;
}

TEST_P(GoNetHttpGzip, TransportGzipConcurrentCloseAndRead) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request&) {
        // Only send the gzip header with no body.
        w.Header().Set("Content-Encoding", "gzip");
        w.WriteHeader(200);
        dynamic_cast<http::Flusher&>(w).Flush();
        time::Sleep(2 * time::Second);
    });
    http::Client c = ts.Client();
    if (GetParam() == testMode::http1) {
        // Avoid response body draining in HTTP/1.
        auto tr = make_ptr<http::Transport>();
        tr->DisableKeepAlives = true;
        c.Transport = tr;
    }
    auto [res, err] = c.Get(ts.URL);
    ASSERT_FALSE(err) << err;
    // Read from the response body in a goroutine while it is empty. This
    // will block indefinitely.
    auto readErrCh = make_chan<error>(1);
    auto body = res->Body;
    go([body, readErrCh] {
        byte buf[10];
        readErrCh.send(body->Read(buf).second);
    });
    time::Sleep(50 * time::Millisecond);
    ASSERT_FALSE(res->Body->Close());
    bool got = false;
    select(recv(readErrCh,
                [&](error e) {
                    got = true;
                    EXPECT_TRUE(e) << "Read returned nil error, want error";
                }),
           recv(time::After(1 * time::Second), [](time::Time) {}));
    EXPECT_TRUE(got) << "Read did not unblock on Close";
}

TEST_P(GoNetHttpGzip, TransportRangeAndGzip) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) {
        EXPECT_EQ(r.Header.Get("Accept-Encoding").view().find("gzip"), std::string_view::npos)
            << "Transport advertised gzip support in the Accept header";
        EXPECT_NE(r.Header.Get("Range"), "") << "no Range in request";
    });
    auto [req, _] = http::NewRequest("GET", ts.URL);
    req->Header.Set("Range", "bytes=7-11");
    auto [res, err] = ts.Client().Do(req);
    ASSERT_FALSE(err) << err;
    res->Body->Close();
}

// Make sure we re-use underlying TCP connection for gzipped responses too.
void testTransportReuseConnection_Gzip(GoNetHttpGzip& t, httptest::Server& ts, chan<string> addr) {
    for (int i = 0; i < 2; i++) {
        auto [req, _] = http::NewRequest("GET", ts.URL);
        auto [res, err] = ts.Client().Do(req);
        ASSERT_FALSE(err) << err;
        std::vector<byte> buf(rgz.size());
        auto [n, rerr] = io::ReadFull(*res->Body, buf);
        EXPECT_FALSE(rerr) << i << ". ReadFull = " << n << ", " << rerr;
        // Note: no res.Body.Close call. It should work without it, since the
        // flate.Reader's internal buffering will hit EOF and that should be
        // sufficient.
    }
    auto a1 = addr.recv(), a2 = addr.recv();
    EXPECT_EQ(a1, a2) << "didn't reuse connection";
}

TEST_P(GoNetHttpGzip, TransportReuseConnection_Gzip_Chunked) {
    auto addr = make_chan<string>(2);
    auto& ts = serve([addr](http::ResponseWriter& w, http::Request& r) {
        addr.send(r.RemoteAddr);
        w.Header().Set("Content-Encoding", "gzip");
        dynamic_cast<http::Flusher&>(w).Flush();
        w.Write(rgz);  // arbitrary gzip response
    });
    testTransportReuseConnection_Gzip(*this, ts, addr);
}

TEST_P(GoNetHttpGzip, TransportReuseConnection_Gzip_ContentLength) {
    auto addr = make_chan<string>(2);
    auto& ts = serve([addr](http::ResponseWriter& w, http::Request& r) {
        addr.send(r.RemoteAddr);
        w.Header().Set("Content-Encoding", "gzip");
        w.Write(rgz);  // arbitrary gzip response
    });
    testTransportReuseConnection_Gzip(*this, ts, addr);
}

// go++: with DisableCompression, or an Accept-Encoding of the caller's, the
// body stays compressed; the caller's request isn't changed either way.
TEST_P(GoNetHttpGzip, TransportDisableCompression) {
    auto& ts = serve([](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("X-Accept-Encoding", r.Header.Get("Accept-Encoding"));
        w.Header().Set("Content-Encoding", "gzip");
        w.Write(rgz);
    });
    auto [req, _] = http::NewRequest("GET", ts.URL);
    auto [res, err] = ts.Client().Do(req);
    ASSERT_FALSE(err) << err;
    io::ReadAll(*res->Body);
    res->Body->Close();
    EXPECT_EQ(res->Header.Get("X-Accept-Encoding"), "gzip");
    EXPECT_FALSE(req->Header.Has("Accept-Encoding")) << "the caller's request changed";

    // The caller's own Accept-Encoding.
    auto [req2, _2] = http::NewRequest("GET", ts.URL);
    req2->Header.Set("Accept-Encoding", "gzip");
    std::tie(res, err) = ts.Client().Do(req2);
    ASSERT_FALSE(err) << err;
    auto [body, _3] = io::ReadAll(*res->Body);
    res->Body->Close();
    EXPECT_FALSE(res->Uncompressed);
    EXPECT_EQ(res->Header.Get("Content-Encoding"), "gzip");
    EXPECT_EQ(std::vector<byte>(body.begin(), body.end()), rgz);

    // DisableCompression.
    auto tr = dynamic_pointer_cast<http::Transport>(ts.Client().Transport);
    ASSERT_TRUE(tr);
    tr->DisableCompression = true;
    std::tie(res, err) = ts.Client().Get(ts.URL);
    tr->DisableCompression = false;
    ASSERT_FALSE(err) << err;
    io::ReadAll(*res->Body);
    res->Body->Close();
    EXPECT_EQ(res->Header.Get("X-Accept-Encoding"), "");
    EXPECT_EQ(res->Header.Get("Content-Encoding"), "gzip");
    EXPECT_FALSE(res->Uncompressed);
}

}  // namespace
