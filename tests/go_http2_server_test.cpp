// Ported from Go 1.27.3's net/http/internal/http2/server_test.go: a
// serverTester speaks raw frames to the server over h2c, and the tests keep
// Go's names. Left out: TLS version and cipher tests (OpenSSL's job), server
// push, RFC 9218 priorities, trailers, and tests of Go internals
// (goroutine counts, buffer retention, synctest timing).
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

class server_tester {
public:
    explicit server_tester(http::handler_fn h, std::function<void(http::Server&)> configure = nullptr) {
        http::Protocols p;
        p.SetHTTP1(true);
        p.SetUnencryptedHTTP2(true);
        srv.Handler = make_ptr<http::HandlerFunc>(std::move(h));
        srv.Protocols = p;
        if (configure) configure(srv);
        auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
        EXPECT_FALSE(err) << err;
        done = makeChan<gopp::error>(1);
        go([this, ln] { done.send(srv.Serve(ln)); });
        auto [c, derr] = net::Dial("tcp", ln->Addr()->String());
        EXPECT_FALSE(derr) << derr;
        cc = c;
        br = make_ptr<bufio::Reader>(cc, 1 << 16);
        fr = std::make_unique<h2::Framer>(br);
        fr->read_meta_headers = &dec;
    }

    ~server_tester() {
        if (cc) cc->Close();
        srv.Close();
        done.recv();
    }

    void writeRaw(std::string_view s) {
        auto [n, err] = io::WriteString(*cc, s);
        ASSERT_FALSE(err) << err;
    }

    void writePreface() { writeRaw(h2::client_preface); }

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
        server_settings = f.settings;
        std::string ack;
        h2::appendSettingsAck(ack);
        writeRaw(ack);
        // Expect the ack of our SETTINGS; the server's connection window
        // update may come first.
        for (;;) {
            h2::Frame g = readFrame();
            if (g.type() == FrameType::window_update && g.streamId() == 0) continue;
            ASSERT_EQ(g.type(), FrameType::settings) << g.header.String();
            ASSERT_TRUE(g.isAck());
            break;
        }
    }

    std::string encode(const fields& fs) {
        std::string block;
        enc_out.clear();
        for (auto& [k, v] : fs) enc.writeField({string(k), string(v)});
        block.swap(enc_out);
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
        h2::appendHeaders(out, id, encode(fs), endStream, h2::initial_max_frame_size);
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
            if (f.type() == FrameType::window_update || f.type() == FrameType::settings || f.type() == FrameType::ping)
                continue;
            // A handler done before the request body: the server ends the
            // stream with RST_STREAM(NO_ERROR) after the response, as Go's.
            // DATA still in flight for such a stream gets STREAM_CLOSED.
            if (f.type() == FrameType::rst_stream && (f.err_code == ErrCode::no || f.err_code == ErrCode::stream_closed))
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
        EXPECT_EQ(f.err_code, code) << h2::to_string(f.err_code);
    }

    void wantRst(uint32 id, ErrCode code) {
        h2::Frame f = want(FrameType::rst_stream);
        EXPECT_EQ(f.streamId(), id);
        EXPECT_EQ(f.err_code, code) << h2::to_string(f.err_code);
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
    std::string enc_out;
    hpack::Encoder enc{&enc_out};
    std::vector<h2::Setting> server_settings;
};

// testBodyContents: the handler checks the request body.
void test_body_contents(int64 want_content_length, std::string wantBody, std::function<void(server_tester&)> write,
                        std::string want_read_err = "") {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter& w, http::Request& r) {
        std::string msg;
        if (r.Method != "POST") msg += "method " + r.Method.str() + "; ";
        if (r.ContentLength != want_content_length) msg += "content length " + std::to_string(r.ContentLength) + "; ";
        auto [data, err] = io::ReadAll(*r.Body);
        if (want_read_err.empty()) {
            if (err) msg += "read error " + err.Error().str() + "; ";
            if (asStringView(data) != wantBody) msg += "body " + std::string(asStringView(data)) + "; ";
        } else if (!err || !strings::Contains(err.Error(), want_read_err)) {
            msg += "read error " + (err ? err.Error().str() : std::string("nil")) + ", want " + want_read_err + "; ";
        }
        got.send(msg);
    });
    st.greet();
    write(st);
    std::string msg = got.recv();
    EXPECT_EQ(msg, "");
}

// testRejectRequest: the request is refused with RST_STREAM.
void test_reject_request(fields fs, ErrCode code = ErrCode::protocol) {
    bool ran = false;
    server_tester st([&](http::ResponseWriter&, http::Request&) { ran = true; });
    st.greet();
    st.writeHeaders(1, true, fs);
    st.wantRst(1, code);
    EXPECT_FALSE(ran);
}

// testServerRejectsConn: the connection ends with GOAWAY and code.
void test_server_rejects_conn(std::function<void(server_tester&)> write, ErrCode code = ErrCode::protocol) {
    server_tester st([](http::ResponseWriter&, http::Request&) {});
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

response testResponse(http::handler_fn h, fields req = server_tester::request()) {
    server_tester st(std::move(h));
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
    server_tester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    auto has = [&](h2::SettingID id, uint32 v) {
        for (auto& s : st.server_settings) {
            if (s.id == id) return s.val == v;
        }
        return false;
    };
    EXPECT_TRUE(has(h2::SettingID::max_frame_size, 1 << 20));
    EXPECT_TRUE(has(h2::SettingID::max_concurrent_streams, 250));
    EXPECT_TRUE(has(h2::SettingID::initial_window_size, 1 << 20));
}

TEST(Http2Server, Request_Get) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter& w, http::Request& r) {
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
    st.writeHeaders(1, true, server_tester::request({{"foo-bar", "some-value"}}));
    EXPECT_EQ(got.recv(), "");
    auto h = st.wantHeaders(1, true);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"200"});
}

TEST(Http2Server, Request_Get_PathSlashes) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.RequestURI.str() + " " + r.URL.Path.str()); });
    st.greet();
    st.writeHeaders(1, true, server_tester::request({{":path", "/%2f/"}}));
    EXPECT_EQ(got.recv(), "/%2f/ ///");
}

TEST(Http2Server, Request_Post_NoContentLength_EndStream) {
    test_body_contents(0, "", [](server_tester& st) { st.writeHeaders(1, true, server_tester::request({}, true)); });
}

TEST(Http2Server, Request_Post_Body_ImmediateEOF) {
    test_body_contents(-1, "", [](server_tester& st) {
        st.writeHeaders(1, false, server_tester::request({}, true));
        st.writeData(1, true, "");
    });
}

TEST(Http2Server, Request_Post_Body_OneData) {
    test_body_contents(-1, "Some content", [](server_tester& st) {
        st.writeHeaders(1, false, server_tester::request({}, true));
        st.writeData(1, true, "Some content");
    });
}

TEST(Http2Server, Request_Post_Body_TwoData) {
    test_body_contents(-1, "Some content", [](server_tester& st) {
        st.writeHeaders(1, false, server_tester::request({}, true));
        st.writeData(1, false, "Some ");
        st.writeData(1, true, "content");
    });
}

TEST(Http2Server, Request_Post_Body_ContentLength_Correct) {
    test_body_contents(3, "foo", [](server_tester& st) {
        st.writeHeaders(1, false, server_tester::request({{"content-length", "3"}}, true));
        st.writeData(1, true, "foo");
    });
}

TEST(Http2Server, Request_Post_Body_ContentLength_TooLarge) {
    test_body_contents(3, "", [](server_tester& st) {
        st.writeHeaders(1, false, server_tester::request({{"content-length", "3"}}, true));
        st.writeData(1, true, "12345");
    }, "sender tried to send more than declared Content-Length of 3 bytes");
}

TEST(Http2Server, Request_Post_Body_ContentLength_TooSmall) {
    test_body_contents(4, "", [](server_tester& st) {
        st.writeHeaders(1, false, server_tester::request({{"content-length", "4"}}, true));
        st.writeData(1, true, "123");
    }, "request declared a Content-Length of 4 but only wrote 3 bytes");
}

TEST(Http2Server, Request_Post_Body_ContentLength_EndStream) {
    // A Content-Length with END_STREAM on the HEADERS: malformed.
    test_reject_request(server_tester::request({{"content-length", "3"}}, true));
}

TEST(Http2Server, Request_Get_Host) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Host.str()); });
    st.greet();
    st.writeHeaders(1, true, server_tester::request({{":authority", "\x7f"}, {"host", "foo.com"}}));
    EXPECT_EQ(got.recv(), "foo.com");
}

TEST(Http2Server, Request_Get_Authority) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Host.str()); });
    st.greet();
    st.writeHeaders(1, true, server_tester::request({{":authority", "foo.com"}}));
    EXPECT_EQ(got.recv(), "foo.com");
}

TEST(Http2Server, Request_WithContinuation) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Header.Get("Foo").str() + r.Header.Get("Bar").str()); });
    st.greet();
    std::string block = st.encode(server_tester::request({{"foo", std::string(100, 'a')}, {"bar", std::string(100, 'b')}}));
    // Split into a HEADERS and two CONTINUATIONs.
    std::size_t a = block.size() / 3, b = 2 * block.size() / 3;
    st.writeFrame(FrameType::headers, h2::flags::endStream, 1, block.substr(0, a));
    st.writeFrame(FrameType::continuation, 0, 1, block.substr(a, b - a));
    st.writeFrame(FrameType::continuation, h2::flags::end_headers, 1, block.substr(b));
    EXPECT_EQ(got.recv(), std::string(100, 'a') + std::string(100, 'b'));
}

TEST(Http2Server, Request_CookieConcat) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Header.Get("Cookie").str()); });
    st.greet();
    st.writeHeaders(1, true, server_tester::request({{"cookie", "a=b"}, {"cookie", "c=d"}, {"cookie", "e=f"}}));
    EXPECT_EQ(got.recv(), "a=b; c=d; e=f");
}

TEST(Http2Server, Request_Reject_CapitalHeader) { test_reject_request(server_tester::request({{"UPPER", "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldNameColon) { test_reject_request(server_tester::request({{"has:colon", "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldNameNULL) { test_reject_request(server_tester::request({{std::string("has\0null", 8), "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldNameEmpty) { test_reject_request(server_tester::request({{"", "v"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldValueNewline) { test_reject_request(server_tester::request({{"foo", "bar\nbaz"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldValueCR) { test_reject_request(server_tester::request({{"foo", "bar\rbaz"}})); }
TEST(Http2Server, Request_Reject_HeaderFieldValueDEL) { test_reject_request(server_tester::request({{"foo", "bar\x7f" "baz"}})); }
TEST(Http2Server, Request_Reject_Pseudo_Missing_method) { test_reject_request(server_tester::request({{":method", "\x7f"}})); }
TEST(Http2Server, Request_Reject_Pseudo_ExactlyOne) {
    // A duplicate pseudo header.
    fields fs = server_tester::request();
    fs.emplace_back(":method", "GET");
    test_reject_request(fs);
}
TEST(Http2Server, Request_Reject_Pseudo_AfterRegular) {
    test_reject_request({{":method", "GET"}, {"foo", "bar"}, {":scheme", "https"}, {":path", "/"}, {":authority", "x"}});
}
TEST(Http2Server, Request_Reject_Pseudo_Missing_path) { test_reject_request(server_tester::request({{":path", "\x7f"}})); }
TEST(Http2Server, Request_Reject_Pseudo_Missing_scheme) { test_reject_request(server_tester::request({{":scheme", "\x7f"}})); }
TEST(Http2Server, Request_Reject_Pseudo_scheme_invalid) { test_reject_request(server_tester::request({{":scheme", "bogus"}})); }
TEST(Http2Server, Request_Reject_Pseudo_Unknown) { test_reject_request(server_tester::request({{":unknown_thing", ""}})); }

TEST(Http2Server, RejectFrameOnIdle_WindowUpdate) {
    test_server_rejects_conn([](server_tester& st) {
        std::string out;
        h2::appendWindowUpdate(out, 123, 456);
        st.writeRaw(out);
    });
}

TEST(Http2Server, RejectFrameOnIdle_Data) {
    test_server_rejects_conn([](server_tester& st) { st.writeData(123, true, "foo"); });
}

TEST(Http2Server, RejectFrameOnIdle_RSTStream) {
    test_server_rejects_conn([](server_tester& st) {
        std::string out;
        h2::appendRstStream(out, 123, ErrCode::cancel);
        st.writeRaw(out);
    });
}

TEST(Http2Server, Request_Connect) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) { got.send(r.Method.str() + " " + r.Host.str()); });
    st.greet();
    st.writeHeaders(1, true, {{":method", "CONNECT"}, {":authority", "example.com:123"}});
    EXPECT_EQ(got.recv(), "CONNECT example.com:123");
}

TEST(Http2Server, Request_Connect_InvalidPath) {
    test_reject_request({{":method", "CONNECT"}, {":authority", "example.com:123"}, {":path", "/bogus"}});
}

TEST(Http2Server, Request_Connect_InvalidScheme) {
    test_reject_request({{":method", "CONNECT"}, {":authority", "example.com:123"}, {":scheme", "https"}});
}

TEST(Http2Server, Ping) {
    server_tester st([](http::ResponseWriter&, http::Request&) {});
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
    test_server_rejects_conn([](server_tester& st) {
        std::string out;
        h2::appendFrameHeader(out, FrameType::data, 0, 1, (1 << 20) + 1);
        st.writeRaw(out);
        st.writeRaw(std::string((1 << 20) + 1, 'x'));
    }, ErrCode::frame_size);
}

TEST(Http2Server, Handler_Sends_WindowUpdate) {
    auto read = makeChan<int>(1);
    auto proceed = makeChan<bool>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) {
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
    st.writeHeaders(1, false, server_tester::request({}, true));
    st.writeData(1, false, std::string(1 << 15, 'a'));
    st.writeData(1, false, std::string(1 << 15, 'b'));
    EXPECT_EQ(read.recv(), 1 << 16);
    // The server returns what the handler read, to the connection and the
    // stream.
    bool conn = false, stream = false;
    while (!conn || !stream) {
        h2::Frame f = st.want(FrameType::window_update);
        if (f.streamId() == 0) conn = true;
        if (f.streamId() == 1) stream = true;
    }
    proceed.send(true);
}

TEST(Http2Server, Send_GoAway_After_Bogus_WindowUpdate) {
    test_server_rejects_conn([](server_tester& st) {
        std::string out;
        h2::appendWindowUpdate(out, 0, 0x7fffffff);  // overflows the connection's window
        st.writeRaw(out);
    }, ErrCode::flow_control);
}

TEST(Http2Server, Send_RstStream_After_Bogus_WindowUpdate) {
    auto unblock = makeChan<bool>(1);
    server_tester st([&](http::ResponseWriter&, http::Request&) { unblock.recv(); });
    st.greet();
    st.writeHeaders(1, false, server_tester::request({}, true));
    std::string out;
    h2::appendWindowUpdate(out, 1, 0x7fffffff);  // overflows the stream's window
    st.writeRaw(out);
    st.wantRst(1, ErrCode::flow_control);
    unblock.send(true);
}

TEST(Http2Server, RSTStream_Unblocks_Read) {
    auto got = makeChan<std::string>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) {
        byte b[1];
        auto [n, err] = r.Body->Read(b);
        got.send(err ? err.Error().str() : std::string("nil"));
    });
    st.greet();
    st.writeHeaders(1, false, server_tester::request({}, true));
    std::string out;
    h2::appendRstStream(out, 1, ErrCode::cancel);
    st.writeRaw(out);
    EXPECT_EQ(got.recv(), "stream error: stream ID 1; CANCEL");
}

TEST(Http2Server, DeadConn_Unblocks_Read) {
    auto got = makeChan<bool>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) {
        byte b[1];
        auto [n, err] = r.Body->Read(b);
        got.send(static_cast<bool>(err));
    });
    st.greet();
    st.writeHeaders(1, false, server_tester::request({}, true));
    st.cc->Close();
    EXPECT_TRUE(got.recv());
}

TEST(Http2Server, CloseNotify_After_RSTStream) {
    auto done = makeChan<bool>(1);
    server_tester st([&](http::ResponseWriter&, http::Request& r) {
        r.Context()->Done().recv();
        done.send(true);
    });
    st.greet();
    st.writeHeaders(1, false, server_tester::request({}, true));
    std::string out;
    h2::appendRstStream(out, 1, ErrCode::cancel);
    st.writeRaw(out);
    EXPECT_TRUE(done.recv());
}

TEST(Http2Server, Rejects_HeadersNoEnd_Then_Headers) {
    test_server_rejects_conn([](server_tester& st) {
        std::string block = st.encode(server_tester::request());
        st.writeFrame(FrameType::headers, h2::flags::endStream, 1, block);
        st.writeFrame(FrameType::headers, h2::flags::endStream | h2::flags::end_headers, 3, block);
    });
}

TEST(Http2Server, Rejects_HeadersNoEnd_Then_Ping) {
    test_server_rejects_conn([](server_tester& st) {
        st.writeFrame(FrameType::headers, h2::flags::endStream, 1, st.encode(server_tester::request()));
        std::string out;
        h2::appendPing(out, false, "foobar!!");
        st.writeRaw(out);
    });
}

TEST(Http2Server, Rejects_HeadersEnd_Then_Continuation) {
    auto got = makeChan<bool>(1);
    server_tester st([&](http::ResponseWriter&, http::Request&) { got.send(true); });
    st.greet();
    st.writeHeaders(1, true, server_tester::request());
    got.recv();
    st.wantHeaders(1, true);
    st.writeFrame(FrameType::continuation, h2::flags::end_headers, 1, "x");
    st.wantGoaway(ErrCode::protocol);
}

TEST(Http2Server, Rejects_HeadersNoEnd_Then_ContinuationWrongStream) {
    test_server_rejects_conn([](server_tester& st) {
        std::string block = st.encode(server_tester::request());
        st.writeFrame(FrameType::headers, h2::flags::endStream, 1, block.substr(0, 3));
        st.writeFrame(FrameType::continuation, h2::flags::end_headers, 3, block.substr(3));
    });
}

TEST(Http2Server, Rejects_Headers0) {
    test_server_rejects_conn([](server_tester& st) {
        st.writeFrame(FrameType::headers, h2::flags::endStream | h2::flags::end_headers, 0, st.encode(server_tester::request()));
    });
}

TEST(Http2Server, Rejects_Continuation0) {
    test_server_rejects_conn([](server_tester& st) { st.writeFrame(FrameType::continuation, h2::flags::end_headers, 0, "x"); });
}

TEST(Http2Server, Rejects_Priority0) {
    test_server_rejects_conn([](server_tester& st) {
        std::string out;
        h2::appendPriority(out, 0, {1, false, 16});
        st.writeRaw(out);
    });
}

TEST(Http2Server, Rejects_HeadersSelfDependence) {
    server_tester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    std::string block = st.encode(server_tester::request());
    std::string payload = std::string("\x00\x00\x00\x01\x0f", 5) + block;  // depends on itself
    st.writeFrame(FrameType::headers, h2::flags::endStream | h2::flags::end_headers | h2::flags::priority, 1, payload);
    st.wantRst(1, ErrCode::protocol);
}

TEST(Http2Server, Rejects_PrioritySelfDependence) {
    server_tester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    std::string out;
    h2::appendPriority(out, 1, {1, false, 16});
    st.writeRaw(out);
    st.wantRst(1, ErrCode::protocol);
}

TEST(Http2Server, Rejects_PushPromise) {
    test_server_rejects_conn([](server_tester& st) {
        st.writeFrame(FrameType::push_promise, h2::flags::end_headers, 1, std::string("\x00\x00\x00\x02", 4) + st.encode(server_tester::request()));
    });
}

TEST(Http2Server, Response_NoData) {
    server_tester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    st.writeHeaders(1, true, server_tester::request());
    auto h = st.wantHeaders(1, true);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"200"});
    EXPECT_EQ(h["content-length"], std::vector<std::string>{"0"});
}

TEST(Http2Server, Response_NoData_Header_FooBar) {
    server_tester st([](http::ResponseWriter& w, http::Request&) { w.Header().Set("Foo-Bar", "some-value"); });
    st.greet();
    st.writeHeaders(1, true, server_tester::request());
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
    server_tester st([&](http::ResponseWriter& w, http::Request&) {
        io::WriteString(w, reply);
        dynamic_cast<http::Flusher&>(w).Flush();
    });
    st.greet({{h2::SettingID::initial_window_size, 0}});
    st.writeHeaders(1, true, server_tester::request());
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
    auto got = makeChan<bool>(1);
    server_tester st([&](http::ResponseWriter& w, http::Request&) {
        std::string big(1 << 20, 'x');
        auto [n, err] = io::WriteString(w, big);
        got.send(static_cast<bool>(err));
    });
    st.greet({{h2::SettingID::initial_window_size, 0}});
    st.writeHeaders(1, true, server_tester::request());
    st.wantHeaders(1, false);
    std::string out;
    h2::appendRstStream(out, 1, ErrCode::cancel);
    st.writeRaw(out);
    EXPECT_TRUE(got.recv());
}

TEST(Http2Server, Response_Automatic100Continue) {
    server_tester st([](http::ResponseWriter& w, http::Request& r) {
        auto [data, err] = io::ReadAll(*r.Body);
        io::WriteString(w, "read " + std::string(asStringView(data)));
    });
    st.greet();
    st.writeHeaders(1, false, server_tester::request({{"expect", "100-continue"}}, true));
    auto h = st.wantHeaders(1, false);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"100"});
    st.writeData(1, true, "some data");
    h = st.wantHeaders(1, false);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"200"});
    EXPECT_EQ(st.wantBody(1), "read some data");
}

TEST(Http2Server, Rejects_Too_Many_Streams) {
    auto unblock = makeChan<bool>(250);
    server_tester st([&](http::ResponseWriter&, http::Request&) { unblock.recv(); });
    st.greet();
    uint32 id = 1;
    for (int i = 0; i < 250; i++, id += 2) st.writeHeaders(id, true, server_tester::request());
    st.writeHeaders(id, true, server_tester::request());
    st.wantRst(id, ErrCode::protocol);
    for (int i = 0; i < 250; i++) unblock.send(true);
}

TEST(Http2Server, Response_ManyHeaders_With_Continuation) {
    server_tester st([](http::ResponseWriter& w, http::Request&) {
        for (int i = 0; i < 5000; i++) w.Header().Set("Foo-" + std::to_string(i), std::to_string(i));
    });
    st.greet();
    st.writeHeaders(1, true, server_tester::request());
    auto h = st.wantHeaders(1, true);
    EXPECT_EQ(h["foo-4999"], std::vector<std::string>{"4999"});
}

TEST(Http2Server, Rejects_ConnHeaders) {
    response r = testResponse([](http::ResponseWriter&, http::Request&) { ADD_FAILURE() << "handler ran"; },
                               server_tester::request({{"connection", "foo"}}));
    EXPECT_EQ(r.header[":status"], std::vector<std::string>{"400"});
    EXPECT_TRUE(strings::Contains(r.body, "request header \"Connection\" is not valid in HTTP/2")) << r.body;
}

TEST(Http2Server, DoS_MaxHeaderListSize) {
    server_tester st([](http::ResponseWriter&, http::Request&) { ADD_FAILURE() << "handler ran"; },
                     [](http::Server& s) { s.MaxHeaderBytes = 1 << 10; });
    st.greet();
    // More than MaxHeaderBytes, in a few CONTINUATIONs.
    fields fs = server_tester::request();
    for (int i = 0; i < 20; i++) fs.emplace_back("x-" + std::to_string(i), std::string(100, 'v'));
    st.writeHeaders(1, true, fs);
    auto h = st.wantHeaders(1, false);
    EXPECT_EQ(h[":status"], std::vector<std::string>{"431"});
}

TEST(Http2Server, NoAutoContentLengthOnHead) {
    server_tester st([](http::ResponseWriter&, http::Request&) {});
    st.greet();
    st.writeHeaders(1, true, server_tester::request({{":method", "HEAD"}}));
    auto h = st.wantHeaders(1, true);
    EXPECT_FALSE(h.contains("content-length"));
}

TEST(Http2Server, HandlerConnectionClose) {
    // Connection: close from a handler starts a graceful shutdown.
    server_tester st([](http::ResponseWriter& w, http::Request&) { w.Header().Set("Connection", "close"); });
    st.greet();
    st.writeHeaders(1, true, server_tester::request());
    bool goaway = false, headers = false;
    while (!goaway || !headers) {
        h2::Frame f = st.readFrame();
        if (f.type() == FrameType::goaway) {
            goaway = true;
            EXPECT_EQ(f.err_code, ErrCode::no);
        }
        if (f.type() == FrameType::headers) {
            headers = true;
            for (auto& hf : f.fields) EXPECT_NE(hf.name, "connection");
        }
    }
    st.wantClosed();
}

TEST(Http2Server, GracefulShutdown) {
    auto entered = makeChan<bool>(1);
    auto proceed = makeChan<bool>(1);
    server_tester st([&](http::ResponseWriter& w, http::Request&) {
        entered.send(true);
        proceed.recv();
        io::WriteString(w, "done");
    });
    st.greet();
    st.writeHeaders(1, true, server_tester::request());
    entered.recv();
    auto shut = makeChan<gopp::error>(1);
    go([&] { shut.send(st.srv.Shutdown(context::Background())); });
    st.wantGoaway(ErrCode::no);
    proceed.send(true);
    st.wantHeaders(1, false);
    EXPECT_EQ(st.wantBody(1), "done");
    st.wantClosed();
    EXPECT_FALSE(shut.recv());
}

TEST(Http2Server, IdleTimeout) {
    server_tester st([](http::ResponseWriter&, http::Request&) {},
                     [](http::Server& s) { s.IdleTimeout = 100 * time::Millisecond; });
    st.greet();
    st.wantGoaway(ErrCode::no);
    st.wantClosed();
}

TEST(Http2Server, UnreadFlowControlReturned_Server) {
    // A handler that doesn't read the body: the connection's window comes
    // back when the stream ends, so other streams can still send.
    server_tester st([](http::ResponseWriter& w, http::Request& r) { io::WriteString(w, r.URL.Path.str()); });
    st.greet();
    for (uint32 id = 1; id < 40; id += 2) {
        st.writeHeaders(id, false, server_tester::request({{":path", "/" + std::to_string(id)}}, true));
        st.writeData(id, true, std::string(16 << 10, 'x'));
        st.wantHeaders(id, false);
        EXPECT_EQ(st.wantBody(id), "/" + std::to_string(id));
    }
}

TEST(Http2Server, Rejects_TooSmall) {
    // A client that doesn't start with SETTINGS.
    server_tester st([](http::ResponseWriter&, http::Request&) {});
    st.writePreface();
    std::string out;
    h2::appendPing(out, false, "12345678");
    st.writeRaw(out);
    st.wantClosed();
}

TEST(Http2Server, InvalidPreface) {
    server_tester st([](http::ResponseWriter&, http::Request&) {});
    st.writeRaw("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");  // the real one, to pass the h2c check
    st.writeRaw("bogus");
    st.wantClosed();
}
