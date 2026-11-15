// Ported from Go 1.27.3's net/http/internal/http2/server_test.go: a
// serverTester speaks raw frames to the server over h2c, and the tests keep
// Go's names, and the server push tests of server_push_test.go, over h2c
// (http:// where Go's use https://). Left out: TLS version and cipher tests
// (OpenSSL's job), and tests of Go internals (goroutine counts, buffer
// retention, stream states, synctest timing). The RFC 9218 priority tests use flow control where Go's
// use synctest: all the streams' responses are queued before the client
// opens their windows, so the order is the scheduler's alone.
#include "net/http/http2.hpp"

#include <gopp/chan.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/net/http.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <map>

using namespace gopp;
namespace h2 = gopp::http::http2;
namespace hpack = gopp::http::hpack;
using h2::ErrCode;
using h2::FrameType;

namespace {

using fields = std::vector<std::pair<std::string, std::string>>;

class serverTester {
public:
    explicit serverTester(http::handlerFn h, std::function<void(http::Server&)> configure = nullptr) {
        http::Protocols p;
        p.SetHTTP1(true);
        p.SetUnencryptedHTTP2(true);
        srv.Handler = make_ptr<http::HandlerFunc>(std::move(h));
        srv.Protocols = p;
        if (configure) configure(srv);
        auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
        EXPECT_FALSE(err) << err;
        done = make_chan<gopp::error>(1);
        go([this, ln] { done.send(srv.Serve(ln)); });
        auto [c, derr] = net::Dial("tcp", ln->Addr()->String());
        EXPECT_FALSE(derr) << derr;
        cc = c;
        br = make_ptr<bufio::Reader>(cc, 1 << 16);
        fr = std::make_unique<h2::Framer>(br);
        fr->readMetaHeaders = &dec;
    }

    ~serverTester() {
        if (cc) cc->Close();
        srv.Close();
        done.recv();
    }

    void writeRaw(std::string_view s) {
        auto [n, err] = io::WriteString(*cc, s);
        ASSERT_FALSE(err) << err;
    }

    void writePreface() { writeRaw(h2::clientPreface); }

    void writeSettings(std::vector<h2::Setting> s = {}) {
        std::string out;
        h2::appendSettings(out, s);
        writeRaw(out);
    }

    // The client preface and SETTINGS, the server's SETTINGS (and its
    // connection window update), and the acks.
    void greet(std::vector<h2::Setting> settings = {}) {
        writePreface();
        writeSettings(settings);
        h2::Frame f = readFrame();
        ASSERT_EQ(f.type(), FrameType::settings) << f.header.String();
        ASSERT_FALSE(f.isAck());
        serverSettings = f.settings;
        std::string ack;
        h2::appendSettingsAck(ack);
        writeRaw(ack);
        // Expect the ack of our SETTINGS; the server's connection window
        // update may come first.
        for (;;) {
            h2::Frame g = readFrame();
            if (g.type() == FrameType::windowUpdate && g.streamId() == 0) continue;
            ASSERT_EQ(g.type(), FrameType::settings) << g.header.String();
            ASSERT_TRUE(g.isAck());
            break;
        }
    }

    std::string encode(const fields& fs) {
        std::string block;
        encOut.clear();
        for (auto& [k, v] : fs) enc.writeField({string(k), string(v)});
        block.swap(encOut);
        return block;
    }

    // A GET request's fields, with replacements and additions.
    static fields request(fields extra = {}, bool post = false) {
        fields fs = {{":method", post ? "POST" : "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}};
        for (auto& [k, v] : extra) {
            bool replaced = false;
            for (auto& [k2, v2] : fs) {
                if (k2 == k && k.starts_with(":")) {
                    v2 = v;
                    replaced = true;
                }
            }
            if (!replaced) fs.emplace_back(k, v);
        }
        // An empty value removes a pseudo header.
        std::erase_if(fs, [](auto& kv) { return kv.first.starts_with(":") && kv.second == "\x7f"; });
        return fs;
    }

    void writeHeaders(uint32 id, bool endStream, const fields& fs) {
        std::string out;
        h2::appendHeaders(out, id, encode(fs), endStream, h2::initialMaxFrameSize);
        writeRaw(out);
    }

    void writeData(uint32 id, bool endStream, std::string_view data) {
        std::string out;
        h2::appendData(out, id, endStream, data);
        writeRaw(out);
    }

    void writeFrame(FrameType t, uint8 flags, uint32 id, std::string_view payload) {
        std::string out;
        h2::appendFrameHeader(out, t, flags, id, payload.size());
        out.append(payload);
        writeRaw(out);
    }

    h2::Frame readFrame() {
        cc->SetReadDeadline(time::Now().Add(5 * time::Second));
        auto [f, err] = fr->readFrame();
        EXPECT_FALSE(err) << err;
        return f;
    }

    // The next frame of type t, skipping WINDOW_UPDATE, SETTINGS and PING.
    h2::Frame want(FrameType t) {
        for (;;) {
            h2::Frame f = readFrame();
            if (f.type() == t) return f;
            if (f.type() == FrameType::windowUpdate || f.type() == FrameType::settings || f.type() == FrameType::ping)
                continue;
            // A handler done before the request body: the server ends the
            // stream with RST_STREAM(NO_ERROR) after the response, as Go's.
            // DATA still in flight for such a stream gets STREAM_CLOSED.
            if (f.type() == FrameType::rstStream && (f.errCode == ErrCode::no || f.errCode == ErrCode::streamClosed))
                continue;
            ADD_FAILURE() << "got " << f.header.String() << ", want " << h2::to_string(t);
            return f;
        }
    }

    // The response header of stream id: the fields as a map of lists.
    std::map<std::string, std::vector<std::string>> wantHeaders(uint32 id, bool endStream) {
        h2::Frame f = want(FrameType::headers);
        EXPECT_EQ(f.streamId(), id);
        EXPECT_EQ(f.streamEnded(), endStream) << f.header.String();
        std::map<std::string, std::vector<std::string>> m;
        for (auto& hf : f.fields) m[hf.name.str()].push_back(hf.value.str());
        return m;
    }

    // DATA of stream id up to END_STREAM.
    std::string wantBody(uint32 id) {
        std::string body;
        for (;;) {
            h2::Frame f = want(FrameType::data);
            EXPECT_EQ(f.streamId(), id);
            body += f.data.view();
            // Keep the windows open.
            if (!f.data.empty()) {
                std::string wu;
                h2::appendWindowUpdate(wu, 0, static_cast<uint32>(f.data.size()));
                h2::appendWindowUpdate(wu, id, static_cast<uint32>(f.data.size()));
                writeRaw(wu);
            }
            if (f.streamEnded()) return body;
        }
    }

    void wantGoaway(ErrCode code) {
        h2::Frame f = want(FrameType::goaway);
        EXPECT_EQ(f.errCode, code) << h2::to_string(f.errCode);
    }

    void wantRst(uint32 id, ErrCode code) {
        h2::Frame f = want(FrameType::rstStream);
        EXPECT_EQ(f.streamId(), id);
        EXPECT_EQ(f.errCode, code) << h2::to_string(f.errCode);
    }

    // The connection ends (after an optional GOAWAY).
    void wantClosed() {
        cc->SetReadDeadline(time::Now().Add(5 * time::Second));
        for (;;) {
            auto [f, err] = fr->readFrame();
            if (err) {
                EXPECT_FALSE(err->Timeout()) << "connection not closed";
                return;
            }
        }
    }

    http::Server srv;
    chan<gopp::error> done;
    ptr<net::Conn> cc;
    ptr<bufio::Reader> br;
    std::unique_ptr<h2::Framer> fr;
    hpack::Decoder dec{4096, nullptr};
    std::string encOut;
    hpack::Encoder enc{&encOut};
    std::vector<h2::Setting> serverSettings;
};

// testBodyContents: the handler checks the request body.
void testBodyContents(int64 wantContentLength, std::string wantBody, std::function<void(serverTester&)> write,
                        std::string wantReadErr = "") {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter& w, http::Request& r) {
        std::string msg;
        if (r.Method != "POST") msg += "method " + r.Method.str() + "; ";
        if (r.ContentLength != wantContentLength) msg += "content length " + std::to_string(r.ContentLength) + "; ";
        auto [data, err] = io::ReadAll(*r.Body);
        if (wantReadErr.empty()) {
            if (err) msg += "read error " + err.Error().str() + "; ";
            if (asStringView(data) != wantBody) msg += "body " + std::string(asStringView(data)) + "; ";
        } else if (!err || !strings::Contains(err.Error(), wantReadErr)) {
            msg += "read error " + (err ? err.Error().str() : std::string("nil")) + ", want " + wantReadErr + "; ";
        }
        got.send(msg);
    });
    st.greet();
    write(st);
    std::string msg = got.recv();
    EXPECT_EQ(msg, "");
}

// testRejectRequest: the request is refused with RST_STREAM.
void testRejectRequest(fields fs, ErrCode code = ErrCode::protocol) {
    bool ran = false;
    serverTester st([&](http::ResponseWriter&, http::Request&) { ran = true; });
    st.greet();
    st.writeHeaders(1, true, fs);
    st.wantRst(1, code);
    EXPECT_FALSE(ran);
}

// testServerRejectsConn: the connection ends with GOAWAY and code.
void testServerRejectsConn(std::function<void(serverTester&)> write, ErrCode code = ErrCode::protocol) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    write(st);
    st.wantGoaway(code);
    st.wantClosed();
}

// A handler's response as the client sees it.
struct response {
    std::map<std::string, std::vector<std::string>> header;
    std::string body;
};

response testResponse(http::handlerFn h, fields req = serverTester::request()) {
    serverTester st(std::move(h));
    st.greet();
    st.writeHeaders(1, true, req);
    response r;
    r.header = st.wantHeaders(1, false);
    r.body = st.wantBody(1);
    return r;
}

}  // namespace

TEST(Http2Server, Server) {
    // The server's SETTINGS: what Go's server announces.
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    auto has = [&](h2::SettingID id, uint32 v) {
        for (auto& s : st.serverSettings) {
            if (s.id == id) return s.val == v;
        }
        return false;
    };
    EXPECT_TRUE(has(h2::SettingID::maxFrameSize, 1 << 20));
    EXPECT_TRUE(has(h2::SettingID::maxConcurrentStreams, 250));
    EXPECT_TRUE(has(h2::SettingID::initialWindowSize, 1 << 20));
}

TEST(Http2Server, Request_Get) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter& w, http::Request& r) {
        std::string msg;
        if (r.Method != "GET") msg += "method; ";
        if (r.URL.Path != "/") msg += "path; ";
        if (r.Proto != "HTTP/2.0" || r.ProtoMajor != 2 || r.ProtoMinor != 0) msg += "proto " + r.Proto.str() + "; ";
        if (r.Host != "example.com") msg += "host " + r.Host.str() + "; ";
        if (r.Header.Get("Foo-Bar") != "some-value") msg += "header; ";
        if (r.RemoteAddr.empty()) msg += "remote addr; ";
        got.send(msg);
    });
    st.greet();
    st.writeHeaders(1, true, serverTester::request({{"foo-bar", "some-value"}}));
    EXPECT_EQ(got.recv(), "");
    auto h = st.wantHeaders(1, true);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"200"});
}

TEST(Http2Server, Request_Get_PathSlashes) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.RequestURI.str() + " " + r.URL.Path.str()); });
    st.greet();
    st.writeHeaders(1, true, serverTester::request({{":path", "/%2f/"}}));
    EXPECT_EQ(got.recv(), "/%2f/ ///");
}

TEST(Http2Server, Request_Post_NoContentLength_EndStream) {
    testBodyContents(0, "", [](serverTester& st) { st.writeHeaders(1, true, serverTester::request({}, true)); });
}

TEST(Http2Server, Request_Post_Body_ImmediateEOF) {
    testBodyContents(-1, "", [](serverTester& st) {
        st.writeHeaders(1, false, serverTester::request({}, true));
        st.writeData(1, true, "");
    });
}

TEST(Http2Server, Request_Post_Body_OneData) {
    testBodyContents(-1, "Some content", [](serverTester& st) {
        st.writeHeaders(1, false, serverTester::request({}, true));
        st.writeData(1, true, "Some content");
    });
}

TEST(Http2Server, Request_Post_Body_TwoData) {
    testBodyContents(-1, "Some content", [](serverTester& st) {
        st.writeHeaders(1, false, serverTester::request({}, true));
        st.writeData(1, false, "Some ");
        st.writeData(1, true, "content");
    });
}

TEST(Http2Server, Request_Post_Body_ContentLength_Correct) {
    testBodyContents(3, "foo", [](serverTester& st) {
        st.writeHeaders(1, false, serverTester::request({{"content-length", "3"}}, true));
        st.writeData(1, true, "foo");
    });
}

TEST(Http2Server, Request_Post_Body_ContentLength_TooLarge) {
    testBodyContents(3, "", [](serverTester& st) {
        st.writeHeaders(1, false, serverTester::request({{"content-length", "3"}}, true));
        st.writeData(1, true, "12345");
    }, "sender tried to send more than declared Content-Length of 3 bytes");
}

TEST(Http2Server, Request_Post_Body_ContentLength_TooSmall) {
    testBodyContents(4, "", [](serverTester& st) {
        st.writeHeaders(1, false, serverTester::request({{"content-length", "4"}}, true));
        st.writeData(1, true, "123");
    }, "request declared a Content-Length of 4 but only wrote 3 bytes");
}

TEST(Http2Server, Request_Post_Body_ContentLength_EndStream) {
    // A Content-Length with END_STREAM on the HEADERS: malformed.
    testRejectRequest(serverTester::request({{"content-length", "3"}}, true));
}

TEST(Http2Server, Request_Get_Host) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Host.str()); });
    st.greet();
    st.writeHeaders(1, true, serverTester::request({{":authority", "\x7f"}, {"host", "foo.com"}}));
    EXPECT_EQ(got.recv(), "foo.com");
}

TEST(Http2Server, Request_Get_Authority) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Host.str()); });
    st.greet();
    st.writeHeaders(1, true, serverTester::request({{":authority", "foo.com"}}));
    EXPECT_EQ(got.recv(), "foo.com");
}

TEST(Http2Server, Request_WithContinuation) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Header.Get("Foo").str() + r.Header.Get("Bar").str()); });
    st.greet();
    std::string block = st.encode(serverTester::request({{"foo", std::string(100, 'a')}, {"bar", std::string(100, 'b')}}));
    // Split into a HEADERS and two CONTINUATIONs.
    std::size_t a = block.size() / 3, b = 2 * block.size() / 3;
    st.writeFrame(FrameType::headers, h2::flags::endStream, 1, block.substr(0, a));
    st.writeFrame(FrameType::continuation, 0, 1, block.substr(a, b - a));
    st.writeFrame(FrameType::continuation, h2::flags::endHeaders, 1, block.substr(b));
    EXPECT_EQ(got.recv(), std::string(100, 'a') + std::string(100, 'b'));
}

TEST(Http2Server, Request_CookieConcat) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Header.Get("Cookie").str()); });
    st.greet();
    st.writeHeaders(1, true, serverTester::request({{"cookie", "a=b"}, {"cookie", "c=d"}, {"cookie", "e=f"}}));
    EXPECT_EQ(got.recv(), "a=b; c=d; e=f");
}

TEST(Http2Server, Request_Reject_CapitalHeader) { testRejectRequest(serverTester::request({{"UPPER", "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldNameColon) { testRejectRequest(serverTester::request({{"has:colon", "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldNameNULL) { testRejectRequest(serverTester::request({{std::string("has\0null", 8), "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldNameEmpty) { testRejectRequest(serverTester::request({{"", "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldValueNewline) { testRejectRequest(serverTester::request({{"foo", "bar\nbaz"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldValueCR) { testRejectRequest(serverTester::request({{"foo", "bar\rbaz"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldValueDEL) { testRejectRequest(serverTester::request({{"foo", "bar\x7f" "baz"}})); }
TEST(Http2Server, Request_Reject_Pseudo_Missing_method) { testRejectRequest(serverTester::request({{":method", "\x7f"}})); }
TEST(Http2Server, Request_Reject_Pseudo_ExactlyOne) {
    // A duplicate pseudo header.
    fields fs = serverTester::request();
    fs.emplace_back(":method", "GET");
    testRejectRequest(fs);
}
TEST(Http2Server, Request_Reject_Pseudo_AfterRegular) {
    testRejectRequest({{":method", "GET"}, {"foo", "bar"}, {":scheme", "https"}, {":path", "/"}, {":authority", "x"}});
}
TEST(Http2Server, Request_Reject_Pseudo_Missing_path) { testRejectRequest(serverTester::request({{":path", "\x7f"}})); }
TEST(Http2Server, Request_Reject_Pseudo_Missing_scheme) { testRejectRequest(serverTester::request({{":scheme", "\x7f"}})); }
TEST(Http2Server, Request_Reject_Pseudo_scheme_invalid) { testRejectRequest(serverTester::request({{":scheme", "bogus"}})); }
TEST(Http2Server, Request_Reject_Pseudo_Unknown) { testRejectRequest(serverTester::request({{":unknown_thing", ""}})); }

TEST(Http2Server, RejectFrameOnIdle_WindowUpdate) {
    testServerRejectsConn([](serverTester& st) {
        std::string out;
        h2::appendWindowUpdate(out, 123, 456);
        st.writeRaw(out);
    });
}

TEST(Http2Server, RejectFrameOnIdle_Data) {
    testServerRejectsConn([](serverTester& st) { st.writeData(123, true, "foo"); });
}

TEST(Http2Server, RejectFrameOnIdle_RSTStream) {
    testServerRejectsConn([](serverTester& st) {
        std::string out;
        h2::appendRstStream(out, 123, ErrCode::cancel);
        st.writeRaw(out);
    });
}

TEST(Http2Server, Request_Connect) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Method.str() + " " + r.Host.str()); });
    st.greet();
    st.writeHeaders(1, true, {{":method", "CONNECT"}, {":authority", "example.com:123"}});
    EXPECT_EQ(got.recv(), "CONNECT example.com:123");
}

TEST(Http2Server, Request_Connect_InvalidPath) {
    testRejectRequest({{":method", "CONNECT"}, {":authority", "example.com:123"}, {":path", "/bogus"}});
}

TEST(Http2Server, Request_Connect_InvalidScheme) {
    testRejectRequest({{":method", "CONNECT"}, {":authority", "example.com:123"}, {":scheme", "https"}});
}

TEST(Http2Server, Ping) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    // An ack must be ignored.
    std::string out;
    h2::appendPing(out, true, "12345678");
    h2::appendPing(out, false, "abcdefgh");
    st.writeRaw(out);
    h2::Frame f = st.want(FrameType::ping);
    EXPECT_TRUE(f.isAck());
    EXPECT_EQ(f.data, "abcdefgh");
}

TEST(Http2Server, RejectsLargeFrames) {
    testServerRejectsConn([](serverTester& st) {
        std::string out;
        h2::appendFrameHeader(out, FrameType::data, 0, 1, (1 << 20) + 1);
        st.writeRaw(out);
        st.writeRaw(std::string((1 << 20) + 1, 'x'));
    }, ErrCode::frameSize);
}

TEST(Http2Server, Handler_Sends_WindowUpdate) {
    auto read = make_chan<int>(1);
    auto proceed = make_chan<bool>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) {
        byte buf[1 << 15];
        int64 total = 0;
        while (total < 1 << 16) {
            auto [n, err] = r.Body->Read(buf);
            if (err) break;
            total += n;
        }
        read.send(static_cast<int>(total));
        proceed.recv();
    });
    st.greet();
    st.writeHeaders(1, false, serverTester::request({}, true));
    st.writeData(1, false, std::string(1 << 15, 'a'));
    st.writeData(1, false, std::string(1 << 15, 'b'));
    EXPECT_EQ(read.recv(), 1 << 16);
    // The server returns what the handler read, to the connection and the
    // stream.
    bool conn = false, stream = false;
    while (!conn || !stream) {
        h2::Frame f = st.want(FrameType::windowUpdate);
        if (f.streamId() == 0) conn = true;
        if (f.streamId() == 1) stream = true;
    }
    proceed.send(true);
}

TEST(Http2Server, Send_GoAway_After_Bogus_WindowUpdate) {
    testServerRejectsConn([](serverTester& st) {
        std::string out;
        h2::appendWindowUpdate(out, 0, 0x7fffffff);  // overflows the connection's window
        st.writeRaw(out);
    }, ErrCode::flowControl);
}

TEST(Http2Server, Send_RstStream_After_Bogus_WindowUpdate) {
    auto unblock = make_chan<bool>(1);
    serverTester st([&](http::ResponseWriter&, http::Request&) { unblock.recv(); });
    st.greet();
    st.writeHeaders(1, false, serverTester::request({}, true));
    std::string out;
    h2::appendWindowUpdate(out, 1, 0x7fffffff);  // overflows the stream's window
    st.writeRaw(out);
    st.wantRst(1, ErrCode::flowControl);
    unblock.send(true);
}

TEST(Http2Server, RSTStream_Unblocks_Read) {
    auto got = make_chan<std::string>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) {
        byte b[1];
        auto [n, err] = r.Body->Read(b);
        got.send(err ? err.Error().str() : std::string("nil"));
    });
    st.greet();
    st.writeHeaders(1, false, serverTester::request({}, true));
    std::string out;
    h2::appendRstStream(out, 1, ErrCode::cancel);
    st.writeRaw(out);
    EXPECT_EQ(got.recv(), "stream error: stream ID 1; CANCEL");
}

TEST(Http2Server, DeadConn_Unblocks_Read) {
    auto got = make_chan<bool>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) {
        byte b[1];
        auto [n, err] = r.Body->Read(b);
        got.send(static_cast<bool>(err));
    });
    st.greet();
    st.writeHeaders(1, false, serverTester::request({}, true));
    st.cc->Close();
    EXPECT_TRUE(got.recv());
}

TEST(Http2Server, CloseNotify_After_RSTStream) {
    auto done = make_chan<bool>(1);
    serverTester st([&](http::ResponseWriter&, http::Request& r) {
        r.Context()->Done().recv();
        done.send(true);
    });
    st.greet();
    st.writeHeaders(1, false, serverTester::request({}, true));
    std::string out;
    h2::appendRstStream(out, 1, ErrCode::cancel);
    st.writeRaw(out);
    EXPECT_TRUE(done.recv());
}

TEST(Http2Server, Rejects_HeadersNoEnd_Then_Headers) {
    testServerRejectsConn([](serverTester& st) {
        std::string block = st.encode(serverTester::request());
        st.writeFrame(FrameType::headers, h2::flags::endStream, 1, block);
        st.writeFrame(FrameType::headers, h2::flags::endStream | h2::flags::endHeaders, 3, block);
    });
}

TEST(Http2Server, Rejects_HeadersNoEnd_Then_Ping) {
    testServerRejectsConn([](serverTester& st) {
        st.writeFrame(FrameType::headers, h2::flags::endStream, 1, st.encode(serverTester::request()));
        std::string out;
        h2::appendPing(out, false, "foobar!!");
        st.writeRaw(out);
    });
}

TEST(Http2Server, Rejects_HeadersEnd_Then_Continuation) {
    auto got = make_chan<bool>(1);
    serverTester st([&](http::ResponseWriter&, http::Request&) { got.send(true); });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    got.recv();
    st.wantHeaders(1, true);
    st.writeFrame(FrameType::continuation, h2::flags::endHeaders, 1, "x");
    st.wantGoaway(ErrCode::protocol);
}

TEST(Http2Server, Rejects_HeadersNoEnd_Then_ContinuationWrongStream) {
    testServerRejectsConn([](serverTester& st) {
        std::string block = st.encode(serverTester::request());
        st.writeFrame(FrameType::headers, h2::flags::endStream, 1, block.substr(0, 3));
        st.writeFrame(FrameType::continuation, h2::flags::endHeaders, 3, block.substr(3));
    });
}

TEST(Http2Server, Rejects_Headers0) {
    testServerRejectsConn([](serverTester& st) {
        st.writeFrame(FrameType::headers, h2::flags::endStream | h2::flags::endHeaders, 0, st.encode(serverTester::request()));
    });
}

TEST(Http2Server, Rejects_Continuation0) {
    testServerRejectsConn([](serverTester& st) { st.writeFrame(FrameType::continuation, h2::flags::endHeaders, 0, "x"); });
}

TEST(Http2Server, Rejects_Priority0) {
    testServerRejectsConn([](serverTester& st) {
        std::string out;
        h2::appendPriority(out, 0, {1, false, 16});
        st.writeRaw(out);
    });
}

TEST(Http2Server, Rejects_HeadersSelfDependence) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    std::string block = st.encode(serverTester::request());
    std::string payload = std::string("\x00\x00\x00\x01\x0f", 5) + block;  // depends on itself
    st.writeFrame(FrameType::headers, h2::flags::endStream | h2::flags::endHeaders | h2::flags::priority, 1, payload);
    st.wantRst(1, ErrCode::protocol);
}

TEST(Http2Server, Rejects_PrioritySelfDependence) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    std::string out;
    h2::appendPriority(out, 1, {1, false, 16});
    st.writeRaw(out);
    st.wantRst(1, ErrCode::protocol);
}

TEST(Http2Server, Rejects_PushPromise) {
    testServerRejectsConn([](serverTester& st) {
        st.writeFrame(FrameType::pushPromise, h2::flags::endHeaders, 1, std::string("\x00\x00\x00\x02", 4) + st.encode(serverTester::request()));
    });
}

TEST(Http2Server, Response_NoData) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    auto h = st.wantHeaders(1, true);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"200"});
    EXPECT_EQ(h["content-length"], std::vector<std::string>{"0"});
}

TEST(Http2Server, Response_NoData_Header_FooBar) {
    serverTester st([](http::ResponseWriter& w, http::Request&) { w.Header().Set("Foo-Bar", "some-value"); });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    auto h = st.wantHeaders(1, true);
    EXPECT_EQ(h["foo-bar"], std::vector<std::string>{"some-value"});
}

TEST(Http2Server, Response_Data_Sniff_DoesntOverride) {
    response r = testResponse([](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Content-Type", "foo/bar");
        io::WriteString(w, "<html>this is HTML.");
    });
    EXPECT_EQ(r.header["content-type"], std::vector<std::string>{"foo/bar"});
    EXPECT_EQ(r.body, "<html>this is HTML.");
}

TEST(Http2Server, Response_TransferEncoding_chunked) {
    response r = testResponse([](http::ResponseWriter& w, http::Request&) {
        w.Header().Set("Transfer-Encoding", "chunked");
        io::WriteString(w, "hi");
    });
    EXPECT_FALSE(r.header.contains("transfer-encoding"));
    EXPECT_EQ(r.body, "hi");
}

TEST(Http2Server, Response_Data_SniffLenType) {
    response r = testResponse([](http::ResponseWriter& w, http::Request&) { io::WriteString(w, "<html>this is HTML."); });
    EXPECT_EQ(r.header[":status"], std::vector<std::string>{"200"});
    EXPECT_EQ(r.header["content-type"], std::vector<std::string>{"text/html; charset=utf-8"});
    EXPECT_EQ(r.header["content-length"], std::vector<std::string>{"19"});
    EXPECT_EQ(r.body, "<html>this is HTML.");
}

TEST(Http2Server, Response_Header_Flush_MidWrite) {
    response r = testResponse([](http::ResponseWriter& w, http::Request&) {
        io::WriteString(w, "foo");
        dynamic_cast<http::Flusher&>(w).Flush();
        io::WriteString(w, "bar");
    });
    EXPECT_FALSE(r.header.contains("content-length"));  // unknown at the flush
    EXPECT_EQ(r.body, "foobar");
}

TEST(Http2Server, Response_LargeWrite) {
    const std::string big(1 << 20, 'x');
    response r = testResponse([&](http::ResponseWriter& w, http::Request&) { io::WriteString(w, big); });
    EXPECT_EQ(r.body.size(), big.size());
    EXPECT_TRUE(r.body == big);
}

TEST(Http2Server, Response_LargeWrite_FlowControlled) {
    // A client window of 0: no DATA until the client opens it.
    const std::string reply = "AAAABBBBCCCC";
    serverTester st([&](http::ResponseWriter& w, http::Request&) {
        io::WriteString(w, reply);
        dynamic_cast<http::Flusher&>(w).Flush();
    });
    st.greet({{h2::SettingID::initialWindowSize, 0}});
    st.writeHeaders(1, true, serverTester::request());
    st.wantHeaders(1, false);
    std::string got;
    for (int i = 0; i < 3; i++) {
        std::string wu;
        h2::appendWindowUpdate(wu, 1, 4);
        st.writeRaw(wu);
        h2::Frame f = st.want(FrameType::data);
        EXPECT_EQ(f.data.size(), 4u);
        got += f.data.view();
    }
    EXPECT_EQ(got, reply);
}

TEST(Http2Server, Response_RST_Unblocks_LargeWrite) {
    auto got = make_chan<bool>(1);
    serverTester st([&](http::ResponseWriter& w, http::Request&) {
        std::string big(1 << 20, 'x');
        auto [n, err] = io::WriteString(w, big);
        got.send(static_cast<bool>(err));
    });
    st.greet({{h2::SettingID::initialWindowSize, 0}});
    st.writeHeaders(1, true, serverTester::request());
    st.wantHeaders(1, false);
    std::string out;
    h2::appendRstStream(out, 1, ErrCode::cancel);
    st.writeRaw(out);
    EXPECT_TRUE(got.recv());
}

TEST(Http2Server, Response_Automatic100Continue) {
    serverTester st([](http::ResponseWriter& w, http::Request& r) {
        auto [data, err] = io::ReadAll(*r.Body);
        io::WriteString(w, "read " + std::string(asStringView(data)));
    });
    st.greet();
    st.writeHeaders(1, false, serverTester::request({{"expect", "100-continue"}}, true));
    auto h = st.wantHeaders(1, false);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"100"});
    st.writeData(1, true, "some data");
    h = st.wantHeaders(1, false);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"200"});
    EXPECT_EQ(st.wantBody(1), "read some data");
}

TEST(Http2Server, Rejects_Too_Many_Streams) {
    auto unblock = make_chan<bool>(250);
    serverTester st([&](http::ResponseWriter&, http::Request&) { unblock.recv(); });
    st.greet();
    uint32 id = 1;
    for (int i = 0; i < 250; i++, id += 2) st.writeHeaders(id, true, serverTester::request());
    st.writeHeaders(id, true, serverTester::request());
    st.wantRst(id, ErrCode::protocol);
    for (int i = 0; i < 250; i++) unblock.send(true);
}

TEST(Http2Server, Response_ManyHeaders_With_Continuation) {
    serverTester st([](http::ResponseWriter& w, http::Request&) {
        for (int i = 0; i < 5000; i++) w.Header().Set("Foo-" + std::to_string(i), std::to_string(i));
    });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    auto h = st.wantHeaders(1, true);
    EXPECT_EQ(h["foo-4999"], std::vector<std::string>{"4999"});
}

TEST(Http2Server, Rejects_ConnHeaders) {
    response r = testResponse([](http::ResponseWriter&, http::Request&) { ADD_FAILURE() << "handler ran"; },
                               serverTester::request({{"connection", "foo"}}));
    EXPECT_EQ(r.header[":status"], std::vector<std::string>{"400"});
    EXPECT_TRUE(strings::Contains(r.body, "request header \"Connection\" is not valid in HTTP/2")) << r.body;
}

TEST(Http2Server, DoS_MaxHeaderListSize) {
    serverTester st([](http::ResponseWriter&, http::Request&) { ADD_FAILURE() << "handler ran"; },
                     [](http::Server& s) { s.MaxHeaderBytes = 1 << 10; });
    st.greet();
    // More than MaxHeaderBytes, in a few CONTINUATIONs.
    fields fs = serverTester::request();
    for (int i = 0; i < 20; i++) fs.emplace_back("x-" + std::to_string(i), std::string(100, 'v'));
    st.writeHeaders(1, true, fs);
    auto h = st.wantHeaders(1, false);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"431"});
}

TEST(Http2Server, NoAutoContentLengthOnHead) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    st.writeHeaders(1, true, serverTester::request({{":method", "HEAD"}}));
    auto h = st.wantHeaders(1, true);
    EXPECT_FALSE(h.contains("content-length"));
}

TEST(Http2Server, HandlerConnectionClose) {
    // Connection: close from a handler starts a graceful shutdown.
    serverTester st([](http::ResponseWriter& w, http::Request&) { w.Header().Set("Connection", "close"); });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    bool goaway = false, headers = false;
    while (!goaway || !headers) {
        h2::Frame f = st.readFrame();
        if (f.type() == FrameType::goaway) {
            goaway = true;
            EXPECT_EQ(f.errCode, ErrCode::no);
        }
        if (f.type() == FrameType::headers) {
            headers = true;
            for (auto& hf : f.fields) EXPECT_NE(hf.name, "connection");
        }
    }
    st.wantClosed();
}

TEST(Http2Server, GracefulShutdown) {
    auto entered = make_chan<bool>(1);
    auto proceed = make_chan<bool>(1);
    serverTester st([&](http::ResponseWriter& w, http::Request&) {
        entered.send(true);
        proceed.recv();
        io::WriteString(w, "done");
    });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    entered.recv();
    auto shut = make_chan<gopp::error>(1);
    go([&] { shut.send(st.srv.Shutdown(context::Background())); });
    st.wantGoaway(ErrCode::no);
    proceed.send(true);
    st.wantHeaders(1, false);
    EXPECT_EQ(st.wantBody(1), "done");
    st.wantClosed();
    EXPECT_FALSE(shut.recv());
}

TEST(Http2Server, IdleTimeout) {
    serverTester st([](http::ResponseWriter&, http::Request&) {},
                     [](http::Server& s) { s.IdleTimeout = 100 * time::Millisecond; });
    st.greet();
    st.wantGoaway(ErrCode::no);
    st.wantClosed();
}

TEST(Http2Server, UnreadFlowControlReturned_Server) {
    // A handler that doesn't read the body: the connection's window comes
    // back when the stream ends, so other streams can still send.
    serverTester st([](http::ResponseWriter& w, http::Request& r) { io::WriteString(w, r.URL.Path.str()); });
    st.greet();
    for (uint32 id = 1; id < 40; id += 2) {
        st.writeHeaders(id, false, serverTester::request({{":path", "/" + std::to_string(id)}}, true));
        st.writeData(id, true, std::string(16 << 10, 'x'));
        st.wantHeaders(id, false);
        EXPECT_EQ(st.wantBody(id), "/" + std::to_string(id));
    }
}

TEST(Http2Server, Rejects_TooSmall) {
    // A client that doesn't start with SETTINGS.
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.writePreface();
    std::string out;
    h2::appendPing(out, false, "12345678");
    st.writeRaw(out);
    st.wantClosed();
}

TEST(Http2Server, InvalidPreface) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.writeRaw("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");  // the real one, to pass the h2c check
    st.writeRaw("bogus");
    st.wantClosed();
}

// ---------------------------------------------------------------------------
// RFC 9218 priorities

TEST(Http2Server, Rejects_PriorityUpdate0) {
    testServerRejectsConn([](serverTester& st) {
        std::string payload(4, '\0');  // prioritized stream 0
        st.writeFrame(FrameType::priorityUpdate, 0, 0, payload);
    });
}

TEST(Http2Server, Rejects_PriorityUpdateUnparsable) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    std::string payload = std::string("\0\0\0\1", 4) + "Invalid dictionary: ((((";
    st.writeFrame(FrameType::priorityUpdate, 0, 0, payload);
    st.wantRst(1, ErrCode::protocol);
}

TEST(Http2Server, SettingNoRFC7540Priorities) {
    serverTester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    bool got = false;
    for (auto& s : st.serverSettings) {
        if (s.id == h2::SettingID::noRfc7540Priorities) got = s.val == 1;
    }
    EXPECT_TRUE(got) << "want SETTINGS_NO_RFC7540_PRIORITIES to be 1";
}

TEST(Http2Server, SettingNoRFC7540PrioritiesInvalid) {
    testServerRejectsConn([](serverTester& st) { st.writeSettings({{h2::SettingID::noRfc7540Priorities, 2}}); });
}

namespace {

constexpr std::size_t priorityBodySize = 128 << 10;

// A server whose handlers each write priorityBodySize bytes at once.
std::unique_ptr<serverTester> newPriorityTester(std::function<void(http::Server&)> configure = nullptr) {
    auto st = std::make_unique<serverTester>(
        [](http::ResponseWriter& w, http::Request&) {
            std::string body(priorityBodySize, 'a');
            io::WriteString(w, body);
        },
        std::move(configure));
    st->greet();
    std::string wu;
    h2::appendWindowUpdate(wu, 0, 1 << 30);
    st->writeRaw(wu);
    return st;
}

// Opens the streams, with a zero window until each has its response
// queued, then opens the windows; returns the stream of each DATA frame
// with data, in order, until all the streams have ended.
std::vector<uint32> frameOrder(serverTester& st, const std::vector<std::pair<uint32, fields>>& reqs) {
    st.writeSettings({{h2::SettingID::initialWindowSize, 0}});
    for (auto& [id, fs] : reqs) st.writeHeaders(id, true, fs);
    for (std::size_t got = 0; got < reqs.size();) {
        h2::Frame f = st.readFrame();
        if (f.type() == FrameType::headers) got++;
        if (f.type() == FrameType::goaway || f.type() == FrameType::rstStream) {
            ADD_FAILURE() << "got " << f.header.String();
            return {};
        }
    }
    // The handlers queue their data right after their headers.
    time::Sleep(50 * time::Millisecond);
    st.writeSettings({{h2::SettingID::initialWindowSize, 1 << 30}});
    std::vector<uint32> order;
    for (std::size_t ended = 0; ended < reqs.size();) {
        h2::Frame f = st.readFrame();
        if (f.type() != FrameType::data) continue;
        if (!f.data.empty()) order.push_back(f.streamId());
        if (f.streamEnded()) ended++;
    }
    return order;
}

// Whether no stream follows itself in the middle half of order: the
// streams took turns (the start and end may not).
bool roundRobin(const std::vector<uint32>& order) {
    for (std::size_t i = order.size() / 4 + 1; i < order.size() * 3 / 4; i++) {
        if (order[i] == order[i - 1]) return false;
    }
    return true;
}

// Whether each stream's frames are together: one stream at a time.
bool oneByOne(const std::vector<uint32>& order) {
    std::vector<uint32> seen;
    for (std::size_t i = 0; i < order.size(); i++) {
        if (i > 0 && order[i] == order[i - 1]) continue;
        if (std::find(seen.begin(), seen.end(), order[i]) != seen.end()) return false;
        seen.push_back(order[i]);
    }
    return true;
}

std::string format(const std::vector<uint32>& order) {
    std::string s;
    for (auto id : order) s += std::to_string(id) + " ";
    return s;
}

}  // namespace

TEST(Http2Server, RFC9218Priority) {
    auto st = newPriorityTester();
    // Eight streams; the larger the id, the more urgent.
    std::vector<std::pair<uint32, fields>> reqs;
    for (int i = 0; i < 8; i++) {
        reqs.push_back({uint32(i * 2 + 1), serverTester::request({{"priority", "u=" + std::to_string(7 - i)}})});
    }
    auto order = frameOrder(*st, reqs);
    std::map<uint32, std::size_t> lastFrame;
    for (std::size_t i = 0; i < order.size(); i++) lastFrame[order[i]] = i;
    for (int i = 0; i < 7; i++) {
        uint32 id = uint32(i * 2 + 1);
        EXPECT_GT(lastFrame[id], lastFrame[id + 2]) << "stream " << id << " finished before stream " << id + 2
                                                     << " unexpectedly: " << format(order);
    }
}

TEST(Http2Server, RFC9218PriorityIgnoredWhenProxied) {
    auto st = newPriorityTester();
    // Priorities through a proxy are ignored: round-robin.
    std::vector<std::pair<uint32, fields>> reqs;
    for (int i = 0; i < 8; i++) {
        reqs.push_back({uint32(i * 2 + 1),
                        serverTester::request({{"priority", "u=" + std::to_string(7 - i)}, {"via", "a proxy"}})});
    }
    auto order = frameOrder(*st, reqs);
    EXPECT_TRUE(roundRobin(order)) << "want round-robin when proxied, got: " << format(order);
}

TEST(Http2Server, RFC9218PriorityAware) {
    auto st = newPriorityTester();
    // A client that hasn't shown it knows RFC 9218: round-robin.
    std::vector<std::pair<uint32, fields>> reqs;
    for (int i = 0; i < 10; i++) reqs.push_back({uint32(i * 2 + 1), serverTester::request()});
    auto order = frameOrder(*st, reqs);
    EXPECT_TRUE(roundRobin(order)) << "want round-robin when unaware of priority, got: " << format(order);

    // A PRIORITY_UPDATE for a finished stream shows the client knows it:
    // now streams of the same urgency, non-incremental by default, are
    // sent one at a time.
    st->writeFrame(FrameType::priorityUpdate, 0, 0, std::string("\0\0\0\1", 4));
    reqs.clear();
    for (int i = 10; i < 20; i++) reqs.push_back({uint32(i * 2 + 1), serverTester::request()});
    order = frameOrder(*st, reqs);
    EXPECT_TRUE(oneByOne(order)) << "want one-by-one when aware of priority, got: " << format(order);
}

TEST(Http2Server, RFC9218PriorityUpdate) {
    auto st = newPriorityTester();
    // All u=3 and non-incremental; then stream 7 becomes the most urgent.
    std::vector<std::pair<uint32, fields>> reqs;
    for (uint32 id : {1u, 3u, 5u, 7u}) reqs.push_back({id, serverTester::request({{"priority", "u=3"}})});
    st->writeSettings({{h2::SettingID::initialWindowSize, 0}});
    for (auto& [id, fs] : reqs) st->writeHeaders(id, true, fs);
    st->writeFrame(FrameType::priorityUpdate, 0, 0, std::string("\0\0\0\7", 4) + "u=0");
    // frameOrder sends the same SETTINGS and HEADERS again otherwise: do
    // its second half here.
    for (std::size_t got = 0; got < reqs.size();) {
        if (st->readFrame().type() == FrameType::headers) got++;
    }
    time::Sleep(50 * time::Millisecond);
    st->writeSettings({{h2::SettingID::initialWindowSize, 1 << 30}});
    std::vector<uint32> order;
    for (std::size_t ended = 0; ended < reqs.size();) {
        h2::Frame f = st->readFrame();
        if (f.type() != FrameType::data) continue;
        if (!f.data.empty()) order.push_back(f.streamId());
        if (f.streamEnded()) ended++;
    }
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order.front(), 7u) << format(order);
    EXPECT_TRUE(oneByOne(order)) << format(order);
}

TEST(Http2Server, DisableClientPriority) {
    auto st = newPriorityTester([](http::Server& s) { s.DisableClientPriority = true; });
    std::vector<std::pair<uint32, fields>> reqs;
    for (int i = 0; i < 8; i++) {
        reqs.push_back({uint32(i * 2 + 1), serverTester::request({{"priority", "u=" + std::to_string(7 - i)}})});
    }
    auto order = frameOrder(*st, reqs);
    EXPECT_TRUE(roundRobin(order)) << "want round-robin with DisableClientPriority, got: " << format(order);
}

// ---------------------------------------------------------------------------
// Server push (server_push_test.go)

namespace {

using headerList = std::vector<std::pair<std::string, std::string>>;

// The fields of a header block, without date.
headerList decodeBlock(serverTester& st, const string& block) {
    auto [fs, err] = st.dec.decodeFull(as_bytes(block.view()));
    EXPECT_FALSE(err) << err;
    headerList out;
    for (auto& hf : fs) {
        if (hf.name != "date") out.emplace_back(hf.name.str(), hf.value.str());
    }
    return out;
}

headerList fieldsOf(const h2::Frame& f) {
    headerList out;
    for (auto& hf : f.fields) {
        if (hf.name != "date") out.emplace_back(hf.name.str(), hf.value.str());
    }
    return out;
}

}  // namespace

TEST(Http2Server, Push_Success) {
    const std::string mainBody = "<html>index page</html>";
    const std::string pushedBody = "<html>pushed page</html>";
    const std::string userAgent = "testagent";
    const std::string cookie = "testcookie";
    const std::string stURL = "http://example.com";

    auto checkPromisedReq = [&](http::Request& r, std::string_view wantMethod, const http::Header& wantH) -> std::string {
        if (r.Method != wantMethod) return "promised Req.Method=" + r.Method.str() + ", want " + std::string(wantMethod);
        if (r.Header.size() != wantH.size()) return "promised Req.Header has " + std::to_string(r.Header.size()) + " fields";
        for (auto& e : wantH) {
            if (r.Header.Get(e.key) != e.value) return "promised Req.Header " + e.key.str() + "=" + r.Header.Get(e.key).str();
        }
        if ("http://" + r.Host.str() != stURL) return "promised Req.Host=" + r.Host.str();
        if (!r.Body) return "nil Body";
        auto [buf, err] = io::ReadAll(*r.Body);
        if (err || !buf.empty()) return "ReadAll(Body) not empty";
        return "";
    };

    auto errc = make_chan<std::string>(3);
    serverTester st([&](http::ResponseWriter& w, http::Request& r) {
        auto& p = dynamic_cast<http::Pusher&>(w);
        std::string uri = r.URL.RequestURI().str();
        if (uri == "/") {
            // Push "/pushed?get" as a GET request, by an absolute URL.
            http::PushOptions opt;
            opt.Header.Set("User-Agent", string(userAgent));
            if (gopp::error err = p.Push(stURL + "/pushed?get", &opt)) {
                errc.send("error pushing /pushed?get: " + err.Error().str());
                return;
            }
            // Push "/pushed?head" as a HEAD request, by a path.
            http::PushOptions opt2;
            opt2.Method = "HEAD";
            opt2.Header.Set("User-Agent", string(userAgent));
            opt2.Header.Set("Cookie", string(cookie));
            if (gopp::error err = p.Push("/pushed?head", &opt2)) {
                errc.send("error pushing /pushed?head: " + err.Error().str());
                return;
            }
            w.Header().Set("Content-Type", "text/html");
            w.Header().Set("Content-Length", strconv::Itoa(static_cast<int64>(mainBody.size())));
            w.WriteHeader(200);
            io::WriteString(w, mainBody);
            errc.send("");
        } else if (uri == "/pushed?get") {
            http::Header wantH;
            wantH.Set("User-Agent", string(userAgent));
            if (auto e = checkPromisedReq(r, "GET", wantH); !e.empty()) {
                errc.send("/pushed?get: " + e);
                return;
            }
            w.Header().Set("Content-Type", "text/html");
            w.Header().Set("Content-Length", strconv::Itoa(static_cast<int64>(pushedBody.size())));
            w.WriteHeader(200);
            io::WriteString(w, pushedBody);
            errc.send("");
        } else if (uri == "/pushed?head") {
            http::Header wantH;
            wantH.Set("User-Agent", string(userAgent));
            wantH.Set("Cookie", string(cookie));
            if (auto e = checkPromisedReq(r, "HEAD", wantH); !e.empty()) {
                errc.send("/pushed?head: " + e);
                return;
            }
            w.WriteHeader(204);
            errc.send("");
        } else {
            errc.send("unknown RequestURL " + uri);
        }
    });

    // One request, which pushes two responses.
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    for (int k = 0; k < 3; k++) {
        std::string err = errc.recv();
        ASSERT_EQ(err, "") << "handler " << k;
    }

    // Stream 1: 2 PUSH_PROMISE, HEADERS, DATA; stream 2: HEADERS, DATA;
    // stream 4: HEADERS.
    std::map<uint32, std::vector<std::function<void(h2::Frame&)>>> expected;
    expected[1] = {
        [&](h2::Frame& f) {
            ASSERT_EQ(f.type(), FrameType::pushPromise) << f.header.String();
            EXPECT_TRUE(f.headersEnded()) << "want END_HEADERS flag in PushPromiseFrame";
            EXPECT_EQ(f.promiseId, 2u);
            EXPECT_EQ(decodeBlock(st, f.data),
                      (headerList{{":method", "GET"}, {":scheme", "http"}, {":authority", "example.com"},
                                  {":path", "/pushed?get"}, {"user-agent", userAgent}}));
        },
        [&](h2::Frame& f) {
            ASSERT_EQ(f.type(), FrameType::pushPromise) << f.header.String();
            EXPECT_EQ(f.promiseId, 4u);
            EXPECT_EQ(decodeBlock(st, f.data),
                      (headerList{{":method", "HEAD"}, {":scheme", "http"}, {":authority", "example.com"},
                                  {":path", "/pushed?head"}, {"cookie", cookie}, {"user-agent", userAgent}}));
        },
        [&](h2::Frame& f) {
            ASSERT_EQ(f.type(), FrameType::headers) << f.header.String();
            EXPECT_EQ(fieldsOf(f), (headerList{{":status", "200"}, {"content-type", "text/html"},
                                               {"content-length", std::to_string(mainBody.size())}}));
        },
        [&](h2::Frame& f) {
            ASSERT_EQ(f.type(), FrameType::data) << f.header.String();
            EXPECT_EQ(f.data, mainBody);
        },
    };
    expected[2] = {
        [&](h2::Frame& f) {
            ASSERT_EQ(f.type(), FrameType::headers) << f.header.String();
            EXPECT_EQ(fieldsOf(f), (headerList{{":status", "200"}, {"content-type", "text/html"},
                                               {"content-length", std::to_string(pushedBody.size())}}));
        },
        [&](h2::Frame& f) {
            ASSERT_EQ(f.type(), FrameType::data) << f.header.String();
            EXPECT_EQ(f.data, pushedBody);
        },
    };
    expected[4] = {
        [&](h2::Frame& f) {
            ASSERT_EQ(f.type(), FrameType::headers) << f.header.String();
            EXPECT_EQ(fieldsOf(f), (headerList{{":status", "204"}}));
        },
    };
    while (!expected.empty()) {
        h2::Frame f = st.readFrame();
        if (f.type() == FrameType::windowUpdate || f.type() == FrameType::settings || f.type() == FrameType::ping) continue;
        uint32 id = f.streamId();
        auto it = expected.find(id);
        ASSERT_TRUE(it != expected.end()) << "unexpected frame " << f.header.String();
        auto check = it->second.front();
        it->second.erase(it->second.begin());
        if (it->second.empty()) expected.erase(it);
        check(f);
    }
}

// The handler of the pushed request can change its header while the
// PUSH_PROMISE is written (Go issue 18326).
TEST(Http2Server, Push_SuccessNoRace) {
    auto errc = make_chan<std::string>(2);
    serverTester st([&](http::ResponseWriter& w, http::Request& r) {
        std::string uri = r.URL.RequestURI().str();
        if (uri == "/") {
            http::PushOptions opt;
            opt.Header.Set("User-Agent", "testagent");
            if (gopp::error err = dynamic_cast<http::Pusher&>(w).Push("/pushed", &opt)) {
                errc.send("error pushing: " + err.Error().str());
                return;
            }
            w.WriteHeader(200);
            errc.send("");
        } else if (uri == "/pushed") {
            r.Header.Set("User-Agent", "newagent");
            r.Header.Set("Cookie", "cookie");
            w.WriteHeader(200);
            errc.send("");
        } else {
            errc.send("unknown RequestURL " + uri);
        }
    });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    for (int k = 0; k < 2; k++) EXPECT_EQ(errc.recv(), "") << "handler " << k;
}

TEST(Http2Server, Push_RejectRecursivePush) {
    auto errc = make_chan<std::string>(3);
    serverTester st([&](http::ResponseWriter& w, http::Request& r) {
        std::string baseURL = "http://" + r.Host.str();
        auto& p = dynamic_cast<http::Pusher&>(w);
        if (r.URL.Path == "/") {
            gopp::error err = p.Push(baseURL + "/push1");
            errc.send(err ? "first Push()=" + err.Error().str() + ", want nil" : "");
        } else if (r.URL.Path == "/push1") {
            gopp::error err = p.Push(baseURL + "/push2");
            errc.send(err == h2::ErrRecursivePush ? "" : "Push()=" + err.Error().str() + ", want ErrRecursivePush");
        } else {
            errc.send("unexpected path: " + r.URL.Path.str());
        }
    });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    EXPECT_EQ(errc.recv(), "") << "First request failed";
    EXPECT_EQ(errc.recv(), "") << "Second request failed";
}

namespace {

// testServer_Push_RejectSingleRequest: the push fails, and no PUSH_PROMISE
// is sent.
void testServerPushRejectSingleRequest(std::function<std::string(http::Pusher&, http::Request&)> doPush,
                                       std::vector<h2::Setting> settings = {}) {
    auto errc = make_chan<std::string>(2);
    serverTester st([&](http::ResponseWriter& w, http::Request& r) { errc.send(doPush(dynamic_cast<http::Pusher&>(w), r)); });
    st.greet();
    if (!settings.empty()) {
        st.writeSettings(settings);
        // The ack.
        for (;;) {
            h2::Frame f = st.readFrame();
            if (f.type() == FrameType::settings && f.isAck()) break;
        }
    }
    st.writeHeaders(1, true, serverTester::request());
    EXPECT_EQ(errc.recv(), "");
    // No PUSH_PROMISE.
    st.wantHeaders(1, true);
}

}  // namespace

TEST(Http2Server, Push_RejectIfDisabled) {
    testServerPushRejectSingleRequest(
        [](http::Pusher& p, http::Request& r) {
            gopp::error err = p.Push("http://" + r.Host.str() + "/pushed");
            return err == http::ErrNotSupported ? std::string() : "Push()=" + err.Error().str() + ", want ErrNotSupported";
        },
        {{h2::SettingID::enablePush, 0}});
}

TEST(Http2Server, Push_RejectWhenNoConcurrentStreams) {
    testServerPushRejectSingleRequest(
        [](http::Pusher& p, http::Request& r) {
            gopp::error err = p.Push("http://" + r.Host.str() + "/pushed");
            return err == h2::ErrPushLimitReached ? std::string() : "Push()=" + err.Error().str() + ", want ErrPushLimitReached";
        },
        {{h2::SettingID::maxConcurrentStreams, 0}});
}

TEST(Http2Server, Push_RejectWrongScheme) {
    testServerPushRejectSingleRequest([](http::Pusher& p, http::Request& r) {
        return p.Push("https://" + r.Host.str() + "/pushed") ? std::string() : "Push() should have failed (push target URL is https)";
    });
}

TEST(Http2Server, Push_RejectMissingHost) {
    testServerPushRejectSingleRequest([](http::Pusher& p, http::Request&) {
        return p.Push("http:pushed") ? std::string() : "Push() should have failed (push target URL missing host)";
    });
}

TEST(Http2Server, Push_RejectRelativePath) {
    testServerPushRejectSingleRequest([](http::Pusher& p, http::Request&) {
        return p.Push("../test") ? std::string() : "Push() should have failed (push target is a relative path)";
    });
}

TEST(Http2Server, Push_RejectForbiddenMethod) {
    testServerPushRejectSingleRequest([](http::Pusher& p, http::Request& r) {
        http::PushOptions opt;
        opt.Method = "POST";
        return p.Push("http://" + r.Host.str() + "/pushed", &opt) ? std::string() : "Push() should have failed (cannot promise a POST)";
    });
}

TEST(Http2Server, Push_RejectForbiddenHeader) {
    testServerPushRejectSingleRequest([](http::Pusher& p, http::Request& r) {
        http::PushOptions opt;
        opt.Header.Set("Content-Length", "10");
        opt.Header.Set("Content-Encoding", "gzip");
        opt.Header.Set("Trailer", "Foo");
        opt.Header.Set("Te", "trailers");
        opt.Header.Set("Host", "test.com");
        opt.Header.Set(":authority", "test.com");
        return p.Push("http://" + r.Host.str() + "/pushed", &opt) ? std::string() : "Push() should have failed (forbidden headers)";
    });
}

TEST(Http2Server, Push_RejectAfterGoAway) {
    auto ready = make_chan<empty>();
    auto errc = make_chan<std::string>(2);
    serverTester st([&](http::ResponseWriter& w, http::Request& r) {
        ready.recv();
        gopp::error err = dynamic_cast<http::Pusher&>(w).Push("http://" + r.Host.str() + "/pushed");
        errc.send(err == http::ErrNotSupported ? std::string() : "Push()=" + (err ? err.Error().str() : std::string("nil")) + ", want ErrNotSupported");
    });
    st.greet();
    st.writeHeaders(1, true, serverTester::request());
    // A GOAWAY, and time for the server to read it.
    std::string ga;
    h2::appendGoaway(ga, 1, ErrCode::no);
    st.writeRaw(ga);
    time::Sleep(50 * time::Millisecond);
    ready.close();
    EXPECT_EQ(errc.recv(), "");
}

// Several requests that push (Go issue 63511).
TEST(Http2Server, Push_Underflow) {
    serverTester st([&](http::ResponseWriter& w, http::Request& r) {
        std::string uri = r.URL.RequestURI().str();
        if (uri == "/") {
            http::PushOptions opt;
            opt.Header.Set("User-Agent", "testagent");
            if (gopp::error err = dynamic_cast<http::Pusher&>(w).Push("/pushed", &opt)) ADD_FAILURE() << "error pushing: " << err;
            w.WriteHeader(200);
        } else if (uri == "/pushed") {
            r.Header.Set("User-Agent", "newagent");
            r.Header.Set("Cookie", "cookie");
            w.WriteHeader(200);
        } else {
            ADD_FAILURE() << "unknown RequestURL " << uri;
        }
    });
    st.greet();
    constexpr int numRequests = 4;
    for (int i = 0; i < numRequests; i++) st.writeHeaders(uint32(1 + i * 2), true, serverTester::request());
    // Each request: one PUSH_PROMISE and two responses.
    int numPushPromises = 0, numHeaders = 0;
    while (numHeaders < numRequests * 2 || numPushPromises < numRequests) {
        h2::Frame f = st.readFrame();
        if (f.type() == FrameType::headers) {
            EXPECT_TRUE(f.streamEnded()) << "got HEADERS frame with no END_STREAM: " << f.header.String();
            numHeaders++;
        } else if (f.type() == FrameType::pushPromise) {
            decodeBlock(st, f.data);  // keep the decoder's table in step
            numPushPromises++;
        } else if (f.type() == FrameType::goaway || f.type() == FrameType::rstStream) {
            FAIL() << "got " << f.header.String();
        }
    }
}
