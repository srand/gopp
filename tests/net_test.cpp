#include <gopp/bufio.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

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

TEST(Net, DialRefused) {
    auto [ln, err] = net::Listen("tcp", "127.0.0.1:0");
    ASSERT_FALSE(err);
    string addr = ln->Addr();
    ln->Close();
    auto [c, derr] = net::Dial("tcp", addr);
    EXPECT_FALSE(c);
    ASSERT_TRUE(derr);
}
