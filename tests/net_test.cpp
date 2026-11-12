#include <gopp/bufio.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace gopp;
using namespace std::chrono_literals;

TEST(Net, SplitHostPort) {
    auto [h, p, err] = net::SplitHostPort("127.0.0.1:80");
    EXPECT_FALSE(err);
    EXPECT_EQ(h, "127.0.0.1");
    EXPECT_EQ(p, "80");
    auto [h6, p6, err6] = net::SplitHostPort("[::1]:8080");
    EXPECT_FALSE(err6);
    EXPECT_EQ(h6, "::1");
    EXPECT_EQ(p6, "8080");
    auto [h0, p0, err0] = net::SplitHostPort(":http");
    EXPECT_FALSE(err0);
    EXPECT_EQ(h0, "");
    EXPECT_EQ(p0, "http");
    auto [hx, px, errx] = net::SplitHostPort("nope");
    EXPECT_TRUE(errx);
    EXPECT_EQ(net::JoinHostPort("::1", "80"), "[::1]:80");
}

TEST(Net, EchoServer) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err) << err;
    string addr = ln->Addr();

    go([ln] {
        for (;;) {
            auto [c, err] = ln->Accept();
            if (err) return;
            go([c] { io::Copy(*c, *c); });
        }
    });

    sync::WaitGroup wg;
    for (int k = 0; k < 20; k++) {
        wg.Go([addr] {
            auto [c, err] = net::Dial("tcp", addr);
            ASSERT_FALSE(err) << err;
            bufio::Reader r(c);
            for (int i = 0; i < 50; i++) {
                fmt::Print(*c, "line {}\n", i);
                auto [line, err] = r.ReadString('\n');
                ASSERT_FALSE(err) << err;
                EXPECT_EQ(line, fmt::Sprint("line {}\n", i));
            }
            c->Close();
        });
    }
    wg.Wait();
    ln->Close();
}

TEST(Net, ReadDeadline) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    auto accepted = make_chan<ptr<net::Conn>>(1);
    go([ln, accepted] { accepted.send(ln->Accept().first); });
    auto [c, derr] = net::Dial("tcp", ln->Addr());
    ASSERT_FALSE(derr);
    auto server_side = accepted.recv();

    c->SetReadDeadline(time::Now().Add(20ms));
    byte buf[16];
    auto start = time::Now();
    auto [n, rerr] = c->Read(buf);
    EXPECT_EQ(n, 0);
    ASSERT_TRUE(rerr);
    EXPECT_TRUE(rerr->Timeout()) << rerr;
    EXPECT_GE(time::Since(start).Milliseconds(), 19);
    ln->Close();
}

TEST(Net, CloseUnblocksReader) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    auto accepted = make_chan<ptr<net::Conn>>(1);
    go([ln, accepted] { accepted.send(ln->Accept().first); });
    auto [c, derr] = net::Dial("tcp", ln->Addr());
    ASSERT_FALSE(derr);
    auto server_side = accepted.recv();

    auto result = make_chan<error>(1);
    go([c, result] {
        byte buf[16];
        result.send(c->Read(buf).second);
    });
    time::Sleep(10ms);
    c->Close();
    error rerr = result.recv();
    ASSERT_TRUE(rerr);
    EXPECT_TRUE(errors::Is(rerr, net::ErrClosed)) << rerr;
    ln->Close();
}

TEST(Net, PeerCloseGivesEOF) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    go([ln] {
        auto [c, err] = ln->Accept();
        if (!err) {
            io::WriteString(*c, "bye");
            c->Close();
        }
    });
    auto [c, derr] = net::Dial("tcp", ln->Addr());
    ASSERT_FALSE(derr);
    auto [data, rerr] = io::ReadAll(*c);
    EXPECT_FALSE(rerr);
    EXPECT_EQ(as_string_view(data), "bye");
    ln->Close();
}

namespace {

// A connected pair: the dialed side and the accepted one.
std::pair<ptr<net::Conn>, ptr<net::Conn>> tcp_pair(const ptr<net::Listener>& ln) {
    auto accepted = make_chan<ptr<net::Conn>>(1);
    go([ln, accepted] { accepted.send(ln->Accept().first); });
    auto [c, err] = net::Dial("tcp", ln->Addr());
    EXPECT_FALSE(err) << err;
    return {c, accepted.recv()};
}

byte pattern(int conn, std::size_t i) { return static_cast<byte>((conn * 131 + i * 7) & 0xff); }

}  // namespace

// Many connections get data that isn't read for a while: with io_uring, it
// fills the rings' buffers until they run out, and then waits in the
// sockets. Every byte must still arrive, in order.
TEST(Net, UnreadDataOnManyConnections) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    constexpr int nconns = 256;
    constexpr std::size_t size = 256 * 1024;
    sync::WaitGroup wg;
    std::atomic<int> bad{0};
    for (int k = 0; k < nconns; k++) {
        auto [c, s] = tcp_pair(ln);
        ASSERT_TRUE(c && s);
        wg.Go([c, k] {
            std::vector<byte> data(size);
            for (std::size_t i = 0; i < size; i++) data[i] = pattern(k, i);
            c->Write(data);
        });
        wg.Go([s, k, &bad] {
            // Reading starts the receive; then the data piles up.
            std::vector<byte> got(size);
            auto [n, rerr] = io::ReadFull(*s, std::span(got).first(1));
            if (rerr) bad++;
            time::Sleep(100ms);
            auto [m, rerr2] = io::ReadFull(*s, std::span(got).subspan(1));
            if (rerr2 || n + m != static_cast<isize>(size)) bad++;
            for (std::size_t i = 0; i < size; i++) {
                if (got[i] != pattern(k, i)) {
                    bad++;
                    break;
                }
            }
            s->Close();
        });
    }
    wg.Wait();
    EXPECT_EQ(bad.load(), 0);
    ln->Close();
}

// A read deadline that passed fails reads, even with data waiting; once
// cleared, the data is there.
TEST(Net, ReadDeadlineKeepsData) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    auto [c, s] = tcp_pair(ln);
    byte buf[16];
    io::WriteString(*s, "a");
    auto [n0, e0] = io::ReadFull(*c, std::span(buf).first(1));
    ASSERT_FALSE(e0) << e0;
    c->SetReadDeadline(time::Now().Add(-1s));
    io::WriteString(*s, "hello");
    time::Sleep(10ms);
    auto [n1, e1] = c->Read(buf);
    EXPECT_EQ(n1, 0);
    ASSERT_TRUE(e1);
    EXPECT_TRUE(e1->Timeout()) << e1;
    c->SetReadDeadline(time::Time{});
    auto [n2, e2] = io::ReadFull(*c, std::span(buf).first(5));
    ASSERT_FALSE(e2) << e2;
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf), 5), "hello");
    ln->Close();
}

// Closing a connection that has been read from, and so has a receive in
// flight with io_uring, closes the socket: the peer reads EOF.
TEST(Net, CloseAfterReadSendsEOF) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    auto [c, s] = tcp_pair(ln);
    io::WriteString(*c, "x");
    byte buf[1];
    auto [n, rerr] = io::ReadFull(*s, buf);
    ASSERT_FALSE(rerr) << rerr;
    s->Close();
    c->SetReadDeadline(time::Now().Add(5s));
    auto [m, eof] = c->Read(buf);
    EXPECT_EQ(m, 0);
    EXPECT_EQ(eof, io::EOF_) << eof;
    ln->Close();
}

TEST(Net, ZeroByteRead) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    auto [c, s] = tcp_pair(ln);
    auto [n, rerr] = c->Read(std::span<byte>());
    EXPECT_EQ(n, 0);
    EXPECT_FALSE(rerr) << rerr;
    ln->Close();
}

TEST(Net, DialRefused) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    string addr = ln->Addr();
    ln->Close();
    auto [c, derr] = net::Dial("tcp", addr);
    EXPECT_FALSE(c);
    ASSERT_TRUE(derr);
}
