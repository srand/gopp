// Ported from Go 1.27.3's net/dial_test.go, rawconn_test.go,
// rawconn_unix_test.go, net_test.go, writev_test.go, pipe_test.go,
// tcpsock_test.go, listen_test.go (TestListenConfigControl),
// tcpconn_keepalive_test.go, sendfile_test.go, splice_linux_test.go,
// mptcpsock_linux_test.go and udpsock_test.go (the AddrPort methods), with
// Go's test names.
//
// Adapted: Go's tests that count system calls through internal/poll's
// hooks (writev, sendfile, splice) check the data that arrives instead;
// the keep-alive tests read the options back with getsockopt instead of
// a hook. Left out: TestDialParallel and the other tests that need
// testHookDialTCP or a blackholed address, and TestPipe, which needs
// golang.org/x/net/nettest.
#include <gopp/bufio.hpp>
#include <gopp/context.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include "net/dial.hpp"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <string>
#include <vector>

using namespace gopp;
using namespace std::chrono_literals;

namespace gopp::net {
template <typename A>
    requires std::is_base_of_v<Addr, A>
std::ostream& operator<<(std::ostream& os, const ptr<A>& a) {
    return os << (a ? a->String().view() : std::string_view("<nil>"));
}
}  // namespace gopp::net

namespace {

std::string str(const string& s) { return std::string(s.view()); }

bool supportsIPv6() {
    static bool ok = [] {
        auto [ln, err] = net::Listen("tcp6", "[::1]:0");
        if (err) return false;
        ln->Close();
        return true;
    }();
    return ok;
}

bool testableNetwork(std::string_view network) {
    if (network.back() == '6') return supportsIPv6();
    return true;
}

// mockserver.go's testUnixAddr: a path for a socket in a new directory.
std::string testUnixAddr() {
    auto [dir, err] = os::MkdirTemp("", "gopp-nettest");
    EXPECT_FALSE(err) << err;
    return std::string(dir.view()) + "/sock";
}

// mockserver.go's newLocalListener.
ptr<net::Listener> newLocalListener(std::string_view network) {
    std::string addr;
    if (network == "tcp" || network == "tcp4") addr = "127.0.0.1:0";
    else if (network == "tcp6") addr = "[::1]:0";
    else addr = testUnixAddr();
    auto [ln, err] = net::Listen(network, addr);
    EXPECT_FALSE(err) << err;
    return ln;
}

// mockserver.go's newLocalPacketListener.
ptr<net::PacketConn> newLocalPacketListener(std::string_view network) {
    std::string addr;
    if (network == "udp" || network == "udp4") addr = "127.0.0.1:0";
    else if (network == "udp6") addr = "[::1]:0";
    else addr = testUnixAddr();
    auto [c, err] = net::ListenPacket(network, addr);
    EXPECT_FALSE(err) << err;
    return c;
}

// rawconn_unix_test.go's controlOnConnSetup.
error controlOnConnSetup(std::string_view network, std::string_view address, syscall::RawConn& c) {
    error operr;
    std::function<void(uintptr)> fn;
    if (network == "tcp" || network == "udp" || network == "ip") {
        return errors::New("ambiguous network: " + std::string(network));
    } else if (network == "unix" || network == "unixpacket" || network == "unixgram") {
        fn = [&](uintptr s) {
            int v;
            socklen_t len = sizeof(v);
            if (::getsockopt(int(s), SOL_SOCKET, SO_ERROR, &v, &len) < 0) operr = syscall::errnoError(errno);
        };
    } else if (network.back() == '4') {
        fn = [&](uintptr s) {
            int one = 1;
            if (::setsockopt(int(s), IPPROTO_IP, IP_TTL, &one, sizeof(one)) < 0) operr = syscall::errnoError(errno);
        };
    } else if (network.back() == '6') {
        fn = [&](uintptr s) {
            int one = 1;
            if (::setsockopt(int(s), IPPROTO_IPV6, IPV6_UNICAST_HOPS, &one, sizeof(one)) < 0) {
                operr = syscall::errnoError(errno);
            }
        };
    } else {
        return errors::New("unknown network: " + std::string(network));
    }
    if (error err = c.Control(fn)) return err;
    return operr;
}

// rawconn_unix_test.go's readRawConn, writeRawConn and controlRawConn.
std::pair<isize, error> readRawConn(syscall::RawConn& c, std::span<byte> b) {
    error operr;
    isize n = 0;
    error err = c.Read([&](uintptr s) {
        ssize_t r = ::read(int(s), b.data(), b.size());
        if (r < 0 && errno == EAGAIN) return false;
        if (r < 0) operr = syscall::errnoError(errno);
        else n = r;
        return true;
    });
    if (err) return {n, err};
    return {n, operr};
}

error writeRawConn(syscall::RawConn& c, std::span<const byte> b) {
    error operr;
    error err = c.Write([&](uintptr s) {
        ssize_t r = ::write(int(s), b.data(), b.size());
        if (r < 0 && errno == EAGAIN) return false;
        if (r < 0) operr = syscall::errnoError(errno);
        return true;
    });
    if (err) return err;
    return operr;
}

error controlRawConn(syscall::RawConn& c, const ptr<net::Addr>& addr) {
    error operr;
    auto fn = [&](uintptr s) {
        int v;
        socklen_t len = sizeof(v);
        if (::getsockopt(int(s), SOL_SOCKET, SO_REUSEADDR, &v, &len) < 0) {
            operr = syscall::errnoError(errno);
            return;
        }
        if (auto* a = dynamic_cast<net::TCPAddr*>(addr.get())) {
            int one = 1;
            int r = 0;
            if (!a->IP.To16().empty() && a->IP.To4().empty()) {
                r = ::setsockopt(int(s), IPPROTO_IPV6, IPV6_UNICAST_HOPS, &one, sizeof(one));
            } else if (a->IP.To16().empty() && !a->IP.To4().empty()) {
                r = ::setsockopt(int(s), IPPROTO_IP, IP_TTL, &one, sizeof(one));
            }
            if (r < 0) operr = syscall::errnoError(errno);
        }
    };
    if (error err = c.Control(fn)) return err;
    return operr;
}

bool isDeadlineExceeded(const error& err) { return errors::Is(err, os::ErrDeadlineExceeded); }

// net_test.go's withTCPConnPair.
void withTCPConnPair(const std::function<error(net::TCPConn&)>& peer1,
                     const std::function<error(net::TCPConn&)>& peer2) {
    auto ln = newLocalListener("tcp");
    auto errc = make_chan<error>(2);
    go([&, ln, errc] {
        auto [c1, err] = ln->Accept();
        if (err) {
            errc.send(err);
            return;
        }
        err = peer1(dynamic_cast<net::TCPConn&>(*c1));
        c1->Close();
        errc.send(err);
    });
    go([&, ln, errc] {
        auto [c2, err] = net::Dial("tcp", ln->Addr()->String().view());
        if (err) {
            errc.send(err);
            return;
        }
        err = peer2(dynamic_cast<net::TCPConn&>(*c2));
        c2->Close();
        errc.send(err);
    });
    for (int i = 0; i < 2; i++) {
        error err = errc.recv();
        EXPECT_FALSE(err) << err;
    }
    ln->Close();
}

// The key of context values in the tests ("id" in Go).
const char idKey = 0;
struct intValue final : object {
    int v;
    explicit intValue(int v_) : v(v_) {}
};

int ctxId(const ptr<context::Context>& ctx) {
    auto v = ctx->Value(&idKey);
    return v ? static_cast<intValue*>(v.get())->v : 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// dial_test.go

TEST(GoNetDial, DialLocal) {
    auto ln = newLocalListener("tcp");
    auto [h, port, err] = net::SplitHostPort(ln->Addr()->String().view());
    ASSERT_FALSE(err);
    auto [c, derr] = net::Dial("tcp", net::JoinHostPort("", port.view()).view());
    ASSERT_FALSE(derr) << derr;
    c->Close();
    ln->Close();
}

TEST(GoNetDial, DialerPartialDeadline) {
    time::Time now = time::Time::Unix(946684800, 0);
    time::Time noDeadline;
    struct {
        time::Time now, deadline;
        int addrs;
        time::Time expectDeadline;
        error expectErr;
    } testCases[] = {
        // Regular division.
        {now, now.Add(12 * time::Second), 1, now.Add(12 * time::Second), nullptr},
        {now, now.Add(12 * time::Second), 2, now.Add(6 * time::Second), nullptr},
        {now, now.Add(12 * time::Second), 3, now.Add(4 * time::Second), nullptr},
        // Bump against the 2-second sane minimum.
        {now, now.Add(12 * time::Second), 999, now.Add(2 * time::Second), nullptr},
        // Total available is now below the sane minimum.
        {now, now.Add(1900 * time::Millisecond), 999, now.Add(1900 * time::Millisecond), nullptr},
        // Null deadline.
        {now, noDeadline, 1, noDeadline, nullptr},
        // Step the clock forward and cross the deadline.
        {now.Add(-1 * time::Millisecond), now, 1, now, nullptr},
        {now.Add(0 * time::Millisecond), now, 1, noDeadline, net::errTimeout},
        {now.Add(1 * time::Millisecond), now, 1, noDeadline, net::errTimeout},
    };
    int i = 0;
    for (auto& tt : testCases) {
        auto [deadline, err] = net::partialDeadline(tt.now, tt.deadline, tt.addrs);
        EXPECT_EQ(err, tt.expectErr) << "#" << i;
        EXPECT_TRUE(deadline.Equal(tt.expectDeadline)) << "#" << i;
        i++;
    }
}

TEST(GoNetDial, DialerLocalAddr) {
    if (!supportsIPv6()) GTEST_SKIP() << "both IPv4 and IPv6 are required";
    struct test {
        std::string network, raddr;
        ptr<net::Addr> laddr;
        bool wantErr;
    };
    auto tcp = [](net::IP ip) { return make_ptr<net::TCPAddr>(ip, 0); };
    std::vector<test> tests = {
        {"tcp4", "127.0.0.1", nullptr, false},
        {"tcp4", "127.0.0.1", make_ptr<net::TCPAddr>(), false},
        {"tcp4", "127.0.0.1", tcp(net::ParseIP("0.0.0.0")), false},
        {"tcp4", "127.0.0.1", tcp(net::ParseIP("0.0.0.0").To4()), false},
        {"tcp4", "127.0.0.1", tcp(net::ParseIP("::")), true},
        {"tcp4", "127.0.0.1", tcp(net::ParseIP("127.0.0.1").To4()), false},
        {"tcp4", "127.0.0.1", tcp(net::ParseIP("127.0.0.1").To16()), false},
        {"tcp4", "127.0.0.1", tcp(net::IPv6loopback), true},
        {"tcp4", "127.0.0.1", make_ptr<net::UDPAddr>(), true},
        {"tcp4", "127.0.0.1", make_ptr<net::UnixAddr>(), true},

        {"tcp6", "::1", nullptr, false},
        {"tcp6", "::1", make_ptr<net::TCPAddr>(), false},
        {"tcp6", "::1", tcp(net::ParseIP("0.0.0.0")), false},
        {"tcp6", "::1", tcp(net::ParseIP("0.0.0.0").To4()), false},
        {"tcp6", "::1", tcp(net::ParseIP("::")), false},
        {"tcp6", "::1", tcp(net::ParseIP("127.0.0.1").To4()), true},
        {"tcp6", "::1", tcp(net::ParseIP("127.0.0.1").To16()), true},
        {"tcp6", "::1", tcp(net::IPv6loopback), false},
        {"tcp6", "::1", make_ptr<net::UDPAddr>(), true},
        {"tcp6", "::1", make_ptr<net::UnixAddr>(), true},

        {"tcp", "127.0.0.1", nullptr, false},
        {"tcp", "127.0.0.1", make_ptr<net::TCPAddr>(), false},
        {"tcp", "127.0.0.1", tcp(net::ParseIP("0.0.0.0")), false},
        {"tcp", "127.0.0.1", tcp(net::ParseIP("0.0.0.0").To4()), false},
        {"tcp", "127.0.0.1", tcp(net::ParseIP("127.0.0.1").To4()), false},
        {"tcp", "127.0.0.1", tcp(net::ParseIP("127.0.0.1").To16()), false},
        {"tcp", "127.0.0.1", tcp(net::IPv6loopback), true},
        {"tcp", "127.0.0.1", make_ptr<net::UDPAddr>(), true},
        {"tcp", "127.0.0.1", make_ptr<net::UnixAddr>(), true},

        {"tcp", "::1", nullptr, false},
        {"tcp", "::1", make_ptr<net::TCPAddr>(), false},
        {"tcp", "::1", tcp(net::ParseIP("0.0.0.0")), false},
        {"tcp", "::1", tcp(net::ParseIP("0.0.0.0").To4()), false},
        {"tcp", "::1", tcp(net::ParseIP("::")), false},
        {"tcp", "::1", tcp(net::ParseIP("127.0.0.1").To4()), true},
        {"tcp", "::1", tcp(net::ParseIP("127.0.0.1").To16()), true},
        {"tcp", "::1", tcp(net::IPv6loopback), false},
        {"tcp", "::1", make_ptr<net::UDPAddr>(), true},
        {"tcp", "::1", make_ptr<net::UnixAddr>(), true},

        // go.dev/issue/34264: with IPv4-mapped addresses, "::" works.
        {"tcp", "127.0.0.1", tcp(net::ParseIP("::")), false},
    };
    ptr<net::Listener> lss[2];
    for (int i = 0; i < 2; i++) {
        lss[i] = newLocalListener(i == 0 ? "tcp4" : "tcp6");
        auto ln = lss[i];
        go([ln] {
            for (;;) {
                auto [c, err] = ln->Accept();
                if (err) return;
                c->Close();
            }
        });
    }
    for (auto& tt : tests) {
        net::Dialer d{.LocalAddr = tt.laddr};
        net::IP ip = net::ParseIP(tt.raddr);
        string addr = !ip.To4().empty() ? lss[0]->Addr()->String() : lss[1]->Addr()->String();
        auto [c, err] = d.Dial(tt.network, addr.view());
        EXPECT_EQ(bool(err), tt.wantErr) << tt.network << " " << tt.laddr << "->" << tt.raddr << ": " << err;
        if (!err) c->Close();
    }
    for (auto& ln : lss) ln->Close();
}

TEST(GoNetDial, CancelAfterDial) {
    auto ln = newLocalListener("tcp");
    sync::WaitGroup wg;
    wg.Add(1);
    // Echo back the first line of each incoming connection.
    go([ln, &wg] {
        for (;;) {
            auto [c, err] = ln->Accept();
            if (err) break;
            bufio::Reader rb(c);
            auto [line, rerr] = rb.ReadString('\n');
            if (rerr) {
                ADD_FAILURE() << rerr;
                c->Close();
                continue;
            }
            auto [n, werr] = c->Write(as_bytes(line.view()));
            EXPECT_FALSE(werr) << werr;
            c->Close();
        }
        wg.Done();
    });
    auto try_ = [&] {
        auto cancel = make_chan<empty>();
        net::Dialer d{.Cancel = cancel};
        auto [c, err] = d.Dial("tcp", ln->Addr()->String().view());
        // Immediately after dialing, request cancellation and sleep. Before
        // go.dev/issue/15078 was fixed, this would cause subsequent
        // operations to fail with an i/o timeout roughly 50% of the time.
        cancel.close();
        time::Sleep(10ms);
        ASSERT_FALSE(err) << err;
        // Send some data to confirm that the connection is still alive.
        const std::string message = "echo!\n";
        auto [n, werr] = c->Write(as_bytes(message));
        ASSERT_FALSE(werr) << werr;
        // The server should echo the line, and close the connection.
        bufio::Reader rb(c);
        auto [line, rerr] = rb.ReadString('\n');
        ASSERT_FALSE(rerr) << rerr;
        EXPECT_EQ(str(line), message);
        auto [b, eerr] = rb.ReadByte();
        EXPECT_EQ(eerr, io::EOF_) << eerr;
        c->Close();
    };
    // The bug manifested about 50% of the time, so try a few times.
    for (int i = 0; i < 10; i++) try_();
    ln->Close();
    wg.Wait();
}

TEST(GoNetDial, DialListenerAddr) {
    auto [ln, err] = net::Listen("tcp4", "localhost:0");
    ASSERT_FALSE(err) << err;
    auto [h, port, serr] = net::SplitHostPort(ln->Addr()->String().view());
    ASSERT_FALSE(serr);
    // An unspecified address, even IPv6's, dials the local system
    // (go.dev/issue/18806).
    std::string dialAddr = "[::]:" + str(port);
    auto [c, derr] = net::Dial("tcp4", dialAddr);
    ASSERT_FALSE(derr) << "Dial(\"tcp4\", " << dialAddr << "): " << derr;
    c->Close();
    ln->Close();
}

TEST(GoNetDial, DialerControl) {
    // StreamDial
    for (std::string network : {"tcp", "tcp4", "tcp6", "unix", "unixpacket"}) {
        if (!testableNetwork(network)) continue;
        auto ln = newLocalListener(network);
        net::Dialer d{.Control = controlOnConnSetup};
        auto [c, err] = d.Dial(network, ln->Addr()->String().view());
        EXPECT_FALSE(err) << network << ": " << err;
        if (c) c->Close();
        ln->Close();
    }
    // PacketDial
    for (std::string network : {"udp", "udp4", "udp6", "unixgram"}) {
        if (!testableNetwork(network)) continue;
        auto c1 = newLocalPacketListener(network);
        net::Dialer d{.Control = controlOnConnSetup};
        auto [c2, err] = d.Dial(network, c1->LocalAddr()->String().view());
        EXPECT_FALSE(err) << network << ": " << err;
        if (c2) c2->Close();
        c1->Close();
        if (network == "unixgram") os::Remove(c1->LocalAddr()->String().view());
    }
}

TEST(GoNetDial, DialerControlContext) {
    int i = 0;
    for (std::string network : {"tcp", "tcp4", "tcp6", "unix", "unixpacket"}) {
        i++;
        if (!testableNetwork(network)) continue;
        auto ln = newLocalListener(network);
        int id = 0;
        net::Dialer d{.ControlContext = [&](const ptr<context::Context>& ctx, std::string_view network,
                                            std::string_view address, syscall::RawConn& c) {
            id = ctxId(ctx);
            return controlOnConnSetup(network, address, c);
        }};
        auto ctx = context::WithValue(context::Background(), &idKey, make_ptr<intValue>(i));
        auto [c, err] = d.DialContext(ctx, network, ln->Addr()->String().view());
        ASSERT_FALSE(err) << network << ": " << err;
        EXPECT_EQ(id, i) << network;
        c->Close();
        ln->Close();
    }
}

TEST(GoNetDial, DialContext) {
    // StreamDial
    int i = 0;
    for (std::string network : {"tcp", "tcp4", "tcp6", "unix", "unixpacket"}) {
        i++;
        if (!testableNetwork(network)) continue;
        auto ln = newLocalListener(network);
        int id = 0;
        net::Dialer d{.ControlContext = [&](const ptr<context::Context>& ctx, std::string_view network,
                                            std::string_view address, syscall::RawConn& c) {
            id = ctxId(ctx);
            return controlOnConnSetup(network, address, c);
        }};
        auto ctx = context::WithValue(context::Background(), &idKey, make_ptr<intValue>(i));
        ptr<net::Conn> c;
        error err;
        if (network.starts_with("tcp")) {
            auto [raddr, perr] = netip::ParseAddrPort(ln->Addr()->String().view());
            ASSERT_FALSE(perr) << perr;
            std::tie(c, err) = d.DialTCP(ctx, network, netip::AddrPort{}, raddr);
        } else {
            auto [raddr, rerr] = net::ResolveUnixAddr(network, ln->Addr()->String().view());
            ASSERT_FALSE(rerr);
            std::tie(c, err) = d.DialUnix(ctx, network, nullptr, raddr);
        }
        EXPECT_FALSE(err) << network << ": " << err;
        EXPECT_EQ(id, i) << network;
        if (c) c->Close();
        ln->Close();
    }
    // PacketDial
    i = 0;
    for (std::string network : {"udp", "udp4", "udp6", "unixgram"}) {
        i++;
        if (!testableNetwork(network)) continue;
        auto c1 = newLocalPacketListener(network);
        int id = 0;
        net::Dialer d{.ControlContext = [&](const ptr<context::Context>& ctx, std::string_view network,
                                            std::string_view address, syscall::RawConn& c) {
            id = ctxId(ctx);
            return controlOnConnSetup(network, address, c);
        }};
        auto ctx = context::WithValue(context::Background(), &idKey, make_ptr<intValue>(i));
        ptr<net::Conn> c2;
        error err;
        if (network.starts_with("udp")) {
            auto [raddr, perr] = netip::ParseAddrPort(c1->LocalAddr()->String().view());
            ASSERT_FALSE(perr) << perr;
            std::tie(c2, err) = d.DialUDP(ctx, network, netip::AddrPort{}, raddr);
        } else {
            auto [raddr, rerr] = net::ResolveUnixAddr(network, c1->LocalAddr()->String().view());
            ASSERT_FALSE(rerr);
            std::tie(c2, err) = d.DialUnix(ctx, network, nullptr, raddr);
        }
        EXPECT_FALSE(err) << network << ": " << err;
        EXPECT_EQ(id, i) << network;
        if (c2) c2->Close();
        c1->Close();
    }
}

TEST(GoNetDial, DialWithNonZeroDeadline) {
    auto ln = newLocalListener("tcp");
    auto [h, port, err] = net::SplitHostPort(ln->Addr()->String().view());
    ASSERT_FALSE(err);
    auto [ctx, cancel] = context::WithDeadline(context::Background(), time::Now().Add(3600 * time::Second));
    net::Dialer dialer;
    auto [c, derr] = dialer.DialContext(ctx, "tcp", net::JoinHostPort("", port.view()).view());
    ASSERT_FALSE(derr) << derr;
    c->Close();
    cancel();
    ln->Close();
}

// go++'s own: a canceled context fails the dial with an error that is
// context::Canceled, and a context that ran out with one that is
// DeadlineExceeded and a timeout.
TEST(GoNetDial, DialContextDone) {
    auto ln = newLocalListener("tcp");
    auto [ctx, cancel] = context::WithCancel(context::Background());
    cancel();
    net::Dialer d;
    auto [c, err] = d.DialContext(ctx, "tcp", ln->Addr()->String().view());
    ASSERT_TRUE(err);
    EXPECT_TRUE(errors::Is(err, context::Canceled)) << err;
    EXPECT_EQ(str(err.Error()).find("operation was canceled") != std::string::npos, true) << err;

    auto [tctx, tcancel] = context::WithDeadline(context::Background(), time::Now().Add(-time::Second));
    auto [c2, err2] = d.DialContext(tctx, "tcp", ln->Addr()->String().view());
    ASSERT_TRUE(err2);
    EXPECT_TRUE(err2->Timeout()) << err2;
    EXPECT_TRUE(errors::Is(err2, context::DeadlineExceeded)) << err2;
    tcancel();
    ln->Close();
}

// ---------------------------------------------------------------------------
// rawconn_test.go

TEST(GoNetDial, RawConnReadWrite) {
    // TCP
    {
        auto ln = newLocalListener("tcp");
        auto done = make_chan<empty>(1);
        go([ln, done] {
            auto [c, err] = ln->Accept();
            if (err) {
                ADD_FAILURE() << err;
                done.send({});
                return;
            }
            auto [cc, serr] = dynamic_cast<net::TCPListener&>(*ln).SyscallConn();
            EXPECT_FALSE(serr);
            bool called = false;
            auto op = [&](uintptr) {
                called = true;
                return true;
            };
            EXPECT_TRUE(cc->Write(op)) << "Write should return an error";
            EXPECT_FALSE(called) << "Write shouldn't call op";
            called = false;
            EXPECT_TRUE(cc->Read(op)) << "Read should return an error";
            EXPECT_FALSE(called) << "Read shouldn't call op";
            byte b[32];
            auto [n, rerr] = c->Read(b);
            if (!rerr) c->Write(std::span<const byte>(b, std::size_t(n)));
            c->Close();
            done.send({});
        });
        auto [c, err] = net::Dial(ln->Addr()->Network().view(), ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        auto [cc, serr] = dynamic_cast<net::TCPConn&>(*c).SyscallConn();
        ASSERT_FALSE(serr);
        std::string data = "HELLO-R-U-THERE";
        ASSERT_FALSE(writeRawConn(*cc, as_bytes(data)));
        byte b[32];
        auto [n, rerr] = readRawConn(*cc, b);
        ASSERT_FALSE(rerr) << rerr;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(b), std::size_t(n)), data);
        done.recv();
        c->Close();
        ln->Close();
    }
    // Deadline
    {
        auto ln = newLocalListener("tcp");
        auto [c, err] = net::Dial(ln->Addr()->Network().view(), ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        auto [cc, serr] = dynamic_cast<net::TCPConn&>(*c).SyscallConn();
        ASSERT_FALSE(serr);
        byte b[1] = {0};
        c->SetDeadline(time::Time{});
        ASSERT_FALSE(c->SetDeadline(time::Now().Add(time::Duration(-1))));
        error werr = writeRawConn(*cc, b);
        ASSERT_TRUE(werr) << "Write should fail";
        EXPECT_TRUE(isDeadlineExceeded(werr)) << werr;
        auto [n, rerr] = readRawConn(*cc, b);
        ASSERT_TRUE(rerr) << "Read should fail";
        EXPECT_TRUE(isDeadlineExceeded(rerr)) << rerr;

        c->SetReadDeadline(time::Time{});
        ASSERT_FALSE(c->SetReadDeadline(time::Now().Add(time::Duration(-1))));
        auto [n2, rerr2] = readRawConn(*cc, b);
        ASSERT_TRUE(rerr2) << "Read should fail";
        EXPECT_TRUE(isDeadlineExceeded(rerr2)) << rerr2;

        c->SetWriteDeadline(time::Time{});
        ASSERT_FALSE(c->SetWriteDeadline(time::Now().Add(time::Duration(-1))));
        error werr2 = writeRawConn(*cc, b);
        ASSERT_TRUE(werr2) << "Write should fail";
        EXPECT_TRUE(isDeadlineExceeded(werr2)) << werr2;
        c->Close();
        ln->Close();
    }
}

TEST(GoNetDial, RawConnControl) {
    auto ln = newLocalListener("tcp");
    auto [cc1, err] = dynamic_cast<net::TCPListener&>(*ln).SyscallConn();
    ASSERT_FALSE(err);
    ASSERT_FALSE(controlRawConn(*cc1, ln->Addr()));
    auto [c, derr] = net::Dial(ln->Addr()->Network().view(), ln->Addr()->String().view());
    ASSERT_FALSE(derr) << derr;
    auto [cc2, serr] = dynamic_cast<net::TCPConn&>(*c).SyscallConn();
    ASSERT_FALSE(serr);
    ASSERT_FALSE(controlRawConn(*cc2, c->LocalAddr()));
    ln->Close();
    EXPECT_TRUE(controlRawConn(*cc1, ln->Addr())) << "Control after Close should fail";
    c->Close();
    EXPECT_TRUE(controlRawConn(*cc2, c->LocalAddr())) << "Control after Close should fail";
}

// ---------------------------------------------------------------------------
// net_test.go

TEST(GoNetDial, CloseRead) {
    for (std::string network : {"tcp", "unix", "unixpacket"}) {
        auto ln = newLocalListener(network);
        auto [c, err] = net::Dial(ln->Addr()->Network().view(), ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        if (auto* t = dynamic_cast<net::TCPConn*>(c.get())) err = t->CloseRead();
        else if (auto* u = dynamic_cast<net::UnixConn*>(c.get())) err = u->CloseRead();
        ASSERT_FALSE(err) << network << ": " << err;
        byte b[1];
        auto [n, rerr] = c->Read(b);
        EXPECT_TRUE(n == 0 && rerr) << network << ": got (" << n << ", " << rerr << "); want (0, error)";
        c->Close();
        ln->Close();
    }
}

TEST(GoNetDial, CloseWrite) {
    for (std::string network : {"tcp", "unix", "unixpacket"}) {
        SCOPED_TRACE(network);
        auto ln = newLocalListener(network);
        auto done = make_chan<empty>(1);
        go([ln, done] {
            auto [c, err] = ln->Accept();
            if (err) {
                ADD_FAILURE() << err;
                done.send({});
                return;
            }
            byte b[1];
            auto [n, rerr] = c->Read(b);
            EXPECT_TRUE(n == 0 && rerr == io::EOF_) << "got (" << n << ", " << rerr << "); want (0, io.EOF)";
            if (auto* t = dynamic_cast<net::TCPConn*>(c.get())) err = t->CloseWrite();
            else if (auto* u = dynamic_cast<net::UnixConn*>(c.get())) err = u->CloseWrite();
            EXPECT_FALSE(err) << err;
            auto [n2, werr] = c->Write(b);
            EXPECT_TRUE(werr) << "got (" << n2 << ", nil); want (any, error)";
            c->Close();
            done.send({});
        });
        auto [c, err] = net::Dial(ln->Addr()->Network().view(), ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        if (auto* t = dynamic_cast<net::TCPConn*>(c.get())) err = t->CloseWrite();
        else if (auto* u = dynamic_cast<net::UnixConn*>(c.get())) err = u->CloseWrite();
        ASSERT_FALSE(err) << err;
        byte b[1];
        auto [n, rerr] = c->Read(b);
        EXPECT_TRUE(n == 0 && rerr == io::EOF_) << "got (" << n << ", " << rerr << "); want (0, io.EOF)";
        auto [n2, werr] = c->Write(b);
        EXPECT_TRUE(werr) << "got (" << n2 << ", nil); want (any, error)";
        done.recv();
        c->Close();
        ln->Close();
    }
}

TEST(GoNetDial, ConnClose) {
    for (std::string network : {"tcp", "unix", "unixpacket"}) {
        auto ln = newLocalListener(network);
        auto [c, err] = net::Dial(ln->Addr()->Network().view(), ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        ASSERT_FALSE(c->Close());
        byte b[1];
        auto [n, rerr] = c->Read(b);
        EXPECT_TRUE(n == 0 && rerr) << network;
        ln->Close();
    }
}

TEST(GoNetDial, ListenerClose) {
    for (std::string network : {"tcp", "unix", "unixpacket"}) {
        auto ln = newLocalListener(network);
        ASSERT_FALSE(ln->Close());
        auto [c, err] = ln->Accept();
        EXPECT_TRUE(err) << network << ": should fail";
        if (c) c->Close();
    }
}

TEST(GoNetDial, PacketConnClose) {
    for (std::string network : {"udp", "unixgram"}) {
        auto c = newLocalPacketListener(network);
        ASSERT_FALSE(c->Close());
        byte b[1];
        auto [n, addr, err] = c->ReadFrom(b);
        EXPECT_TRUE(n == 0 && err) << network;
        if (network == "unixgram") os::Remove(c->LocalAddr()->String().view());
    }
}

TEST(GoNetDial, ReadTimeoutUnblocksRead) {
    auto serverDone = make_chan<empty>();
    auto server = [&](net::TCPConn& cs) -> error {
        auto errc = make_chan<error>(1);
        ptr<net::TCPConn> c(&cs);
        go([c, errc] {
            go([c] {
                time::Sleep(100ms);
                c->SetReadDeadline(time::Time::Unix(123, 0));  // time in the past
            });
            byte buf[1];
            auto [n, err] = c->Read(buf);
            if (n != 0 || !err) errc.send(errors::New("Read = " + std::to_string(n) + ", nil; want 0, non-nil"));
            errc.close();
        });
        error ret;
        // A timer stopped at the end, so that it doesn't outlive the test.
        auto timeout = time::NewTimer(5 * time::Second);
        select(recv(errc, [&](error err) { ret = err; }),
               recv(timeout->C, [&](time::Time) { ret = errors::New("timeout waiting for Read to finish"); }));
        timeout->Stop();
        serverDone.close();
        return ret;
    };
    auto client = [&](net::TCPConn&) -> error {
        serverDone.recv();
        return nullptr;
    };
    withTCPConnPair(client, server);
}

// ---------------------------------------------------------------------------
// writev_test.go

namespace {
slice<byte> bs(std::string_view s) { return toBytes(s); }
}  // namespace

TEST(GoNetDial, Buffers_read) {
    const std::string story = "once upon a time in Gopherland ... ";
    net::Buffers buffers{bs("once "), bs("upon "), bs("a "), bs("time "), bs("in "), bs("Gopherland ... ")};
    auto [got, err] = io::ReadAll(buffers);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(asStringView(got), story);
    EXPECT_EQ(buffers.size(), 0u);
}

TEST(GoNetDial, Buffers_consume) {
    auto render = [](const net::Buffers& b) {
        std::string s = "[";
        for (auto& x : b) s += "\"" + std::string(asStringView(x)) + "\" ";
        return s + "]";
    };
    struct {
        net::Buffers in;
        int64 consume;
        net::Buffers want;
    } tests[] = {
        {{bs("foo"), bs("bar")}, 0, {bs("foo"), bs("bar")}},
        {{bs("foo"), bs("bar")}, 2, {bs("o"), bs("bar")}},
        {{bs("foo"), bs("bar")}, 3, {bs("bar")}},
        {{bs("foo"), bs("bar")}, 4, {bs("ar")}},
        {{{}, {}, {}, bs("bar")}, 1, {bs("ar")}},
        {{{}, {}, {}, bs("foo")}, 0, {bs("foo")}},
        {{{}, {}, {}}, 0, {}},
    };
    int i = 0;
    for (auto& tt : tests) {
        net::Buffers in = tt.in;
        in.consume(tt.consume);
        EXPECT_EQ(render(in), render(tt.want)) << i;
        i++;
    }
}

TEST(GoNetDial, Buffers_WriteTo) {
    for (bool useCopy : {false, true}) {
        for (int chunks : {0, 10, 1023, 1024, 1025}) {
            SCOPED_TRACE((useCopy ? "Copy/" : "WriteTo/") + std::to_string(chunks));
            std::vector<byte> want(static_cast<std::size_t>(chunks));
            for (int i = 0; i < chunks; i++) want[std::size_t(i)] = byte(i);
            withTCPConnPair(
                [&](net::TCPConn& c) -> error {
                    net::Buffers buffers(static_cast<std::size_t>(chunks));
                    for (int i = 0; i < chunks; i++) buffers[std::size_t(i)] = slice<byte>::of(std::span(&want[std::size_t(i)], 1));
                    int64 n;
                    error err;
                    if (useCopy) std::tie(n, err) = io::Copy(c, buffers);
                    else std::tie(n, err) = buffers.WriteTo(c);
                    if (err) return err;
                    if (!buffers.empty()) return errors::New("len(buffers) = " + std::to_string(buffers.size()));
                    if (n != chunks) return errors::New("Buffers.WriteTo returned " + std::to_string(n));
                    return nullptr;
                },
                [&](net::TCPConn& c) -> error {
                    auto [all, err] = io::ReadAll(c);
                    if (err || all.size() != want.size() || !std::equal(all.begin(), all.end(), want.begin())) {
                        return errors::New("client read wrong data");
                    }
                    return nullptr;
                });
        }
    }
}

TEST(GoNetDial, WritevError) {
    auto ln = newLocalListener("tcp");
    auto ch = make_chan<ptr<net::Conn>>(1);
    go([ln, ch] {
        auto [c, err] = ln->Accept();
        EXPECT_FALSE(err) << err;
        ch.send(c);
        ch.close();
    });
    auto [c1, err] = net::Dial("tcp", ln->Addr()->String().view());
    ASSERT_FALSE(err) << err;
    auto c2 = ch.recv();
    ASSERT_TRUE(c2) << "no server side connection";
    c2->Close();
    auto buf = slice<byte>::make(1 << 20);
    net::Buffers buffers(1 << 10, buf);
    auto [n, werr] = buffers.WriteTo(*c1);
    EXPECT_TRUE(werr) << "Buffers.WriteTo(closed conn) succeeded, want error";
    c1->Close();
    ln->Close();
}

// ---------------------------------------------------------------------------
// pipe_test.go

TEST(GoNetDial, PipeCloseError) {
    auto [c1, c2] = net::Pipe();
    c1->Close();
    EXPECT_EQ(c1->Read({}).second, io::ErrClosedPipe);
    EXPECT_EQ(c1->Write({}).second, io::ErrClosedPipe);
    EXPECT_EQ(c1->SetDeadline(time::Time{}), io::ErrClosedPipe);
    EXPECT_EQ(c2->Read({}).second, io::EOF_);
    EXPECT_EQ(c2->Write({}).second, io::ErrClosedPipe);
    EXPECT_EQ(c2->SetDeadline(time::Time{}), io::ErrClosedPipe);
}

// go++'s own, after nettest's basic checks: a write is read in pieces, the
// pipe is synchronous, and deadlines time out.
TEST(GoNetDial, PipeReadWriteDeadline) {
    auto [c1, c2] = net::Pipe();
    go([c1] {
        auto [n, err] = io::WriteString(*c1, "hello, pipe");
        EXPECT_EQ(n, 11);
        EXPECT_FALSE(err);
    });
    byte b[5];
    std::string got;
    while (got.size() < 11) {
        auto [n, err] = c2->Read(b);
        ASSERT_FALSE(err) << err;
        got.append(reinterpret_cast<char*>(b), std::size_t(n));
    }
    EXPECT_EQ(got, "hello, pipe");
    c2->SetReadDeadline(time::Now().Add(20 * time::Millisecond));
    auto [n, err] = c2->Read(b);
    EXPECT_EQ(n, 0);
    ASSERT_TRUE(err);
    EXPECT_TRUE(err->Timeout()) << err;
    EXPECT_TRUE(errors::Is(err, os::ErrDeadlineExceeded)) << err;
    c2->SetReadDeadline(time::Time{});
    c1->SetWriteDeadline(time::Now().Add(-time::Second));
    EXPECT_TRUE(isDeadlineExceeded(c1->Write(b).second));
    c1->Close();
    c2->Close();
}

// ---------------------------------------------------------------------------
// tcpsock_test.go

TEST(GoNetDial, TCPListenerName) {
    for (std::string addr : {"127.0.0.1:0", "[::1]:0"}) {
        if (addr[0] == '[' && !supportsIPv6()) continue;
        auto [ta, err] = net::ResolveTCPAddr("tcp", addr);
        ASSERT_FALSE(err) << err;
        auto [ln, lerr] = net::ListenTCP("tcp", ta);
        ASSERT_FALSE(lerr) << lerr;
        auto la = dynamic_cast<net::TCPAddr*>(ln->Addr().get());
        ASSERT_TRUE(la);
        EXPECT_NE(la->Port, 0) << ln->Addr();
        ln->Close();
    }
}

TEST(GoNetDial, TCPConcurrentAccept) {
    auto ln = newLocalListener("tcp");
    constexpr int N = 10;
    sync::WaitGroup wg;
    wg.Add(N);
    for (int i = 0; i < N; i++) {
        go([ln, &wg] {
            for (;;) {
                auto [c, err] = ln->Accept();
                if (err) break;
                c->Close();
            }
            wg.Done();
        });
    }
    constexpr int attempts = 10 * N;
    std::atomic<int> fails{0};
    net::Dialer d{.Timeout = 200 * time::Millisecond};
    for (int i = 0; i < attempts; i++) {
        auto [c, err] = d.Dial("tcp", ln->Addr()->String().view());
        if (err) fails++;
        else c->Close();
    }
    ln->Close();
    wg.Wait();
    EXPECT_LE(fails.load(), attempts / 10) << "too many dials failed";
}

TEST(GoNetDial, TCPStress) {
    constexpr int conns = 50;
    constexpr int msgLen = 512;
    constexpr int msgs = 1000;
    auto sendMsg = [](net::Conn& c, std::span<byte> buf) {
        auto [n, err] = c.Write(buf);
        if (n != std::ptrdiff_t(buf.size()) || err) ADD_FAILURE() << "Write failed: " << err;
        return !err;
    };
    auto recvMsg = [](net::Conn& c, std::span<byte> buf) {
        for (std::size_t nn = 0; nn < buf.size();) {
            auto [n, err] = c.Read(buf.subspan(nn));
            nn += std::size_t(n);
            if (err) {
                if (err == io::EOF_) return false;
                ADD_FAILURE() << "Read failed: " << err;
                return false;
            }
        }
        return true;
    };
    auto ln = newLocalListener("tcp");
    auto done = make_chan<empty>();
    // Acceptor.
    go([ln, done, recvMsg] {
        for (;;) {
            auto [c, err] = ln->Accept();
            if (err) break;
            // Server connection.
            go([c, recvMsg] {
                std::vector<byte> buf(msgLen);
                while (recvMsg(*c, buf)) {
                }
                c->Close();
            });
        }
        done.close();
    });
    auto sem = make_chan<empty>(conns);
    for (int i = 0; i < conns; i++) {
        // Client connection.
        go([ln, sem, sendMsg] {
            auto [c, err] = net::Dial("tcp", ln->Addr()->String().view());
            if (err) {
                ADD_FAILURE() << "Dial failed: " << err;
                sem.send({});
                return;
            }
            std::vector<byte> buf(msgLen);
            for (int m = 0; m < msgs; m++) sendMsg(*c, buf);
            c->Close();
            sem.send({});
        });
    }
    for (int i = 0; i < conns; i++) sem.recv();
    ln->Close();
    done.recv();
}

TEST(GoNetDial, TCPBig) {
    for (bool writev : {false, true}) {
        SCOPED_TRACE(writev ? "writev" : "write");
        auto ln = newLocalListener("tcp");
        constexpr std::size_t x = std::size_t(1) << 28;  // 256 MiB, enough to exceed any socket buffer
        auto done = make_chan<int64>(1);
        go([ln, done] {
            auto [c, err] = ln->Accept();
            if (err) {
                ADD_FAILURE() << err;
                done.send(0);
                return;
            }
            auto [n, rerr] = io::Copy(io::Discard(), *c);
            EXPECT_FALSE(rerr) << rerr;
            c->Close();
            done.send(n);
        });
        auto [c, err] = net::Dial("tcp", ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        auto buf = slice<byte>::make(x);
        int64 n;
        error werr;
        if (writev) {
            net::Buffers bufs{buf.Sub(0, x / 2), buf.Sub(x / 2)};
            std::tie(n, werr) = bufs.WriteTo(*c);
        } else {
            auto [nn, e] = c->Write(buf);
            n = nn;
            werr = e;
        }
        ASSERT_FALSE(werr) << werr;
        EXPECT_EQ(n, int64(x));
        c->Close();
        EXPECT_EQ(done.recv(), int64(x));
        ln->Close();
    }
}

TEST(GoNetDial, CopyPipeIntoTCP) {
    auto ln = newLocalListener("tcp");
    auto errc = make_chan<error>(1);
    go([ln, errc] {
        auto [c, err] = ln->Accept();
        if (err) {
            errc.send(err);
            return;
        }
        auto [buf, rerr] = io::ReadAll(*c);
        c->Close();
        if (rerr) {
            errc.send(rerr);
            return;
        }
        if (asStringView(buf) != "hello, world") {
            errc.send(errors::New("got \"" + std::string(asStringView(buf)) + "\"; want \"hello, world\""));
            return;
        }
        errc.send(nullptr);
    });
    auto [c, err] = net::Dial("tcp", ln->Addr()->String().view());
    ASSERT_FALSE(err) << err;
    auto [r, w, perr] = os::Pipe();
    ASSERT_FALSE(perr) << perr;
    auto errc2 = make_chan<error>(1);
    go([w, errc2] {
        auto [n, err] = io::WriteString(*w, "hello, world");
        w->Close();
        errc2.send(err);
    });
    auto [n, cerr] = io::Copy(*c, *r);
    ASSERT_FALSE(cerr) << cerr;
    EXPECT_EQ(n, 12);
    c->Close();
    EXPECT_FALSE(errc2.recv());
    EXPECT_FALSE(errc.recv());
    r->Close();
    ln->Close();
}

TEST(GoNetDial, DialTCPDefaultKeepAlive) {
    auto ln = newLocalListener("tcp");
    auto [c, err] = net::DialTCP("tcp", nullptr, ptr<net::TCPAddr>(dynamic_cast<net::TCPAddr*>(ln->Addr().get())));
    ASSERT_FALSE(err) << err;
    auto [rc, serr] = c->SyscallConn();
    ASSERT_FALSE(serr);
    int on = 0, idle = 0;
    rc->Control([&](uintptr s) {
        socklen_t len = sizeof(int);
        ::getsockopt(int(s), SOL_SOCKET, SO_KEEPALIVE, &on, &len);
        ::getsockopt(int(s), IPPROTO_TCP, TCP_KEEPIDLE, &idle, &len);
    });
    EXPECT_EQ(on, 1);
    EXPECT_EQ(idle, 15);  // defaultTCPKeepAliveIdle
    c->Close();
    ln->Close();
}

TEST(GoNetDial, TCPListenAfterClose) {
    // Regression test for go.dev/issue/50216: after a Listener is closed,
    // a goroutine blocked in Accept must return.
    auto ln = newLocalListener("tcp");
    ln->Close();
    for (int i = 0; i < 10; i++) {
        auto [c, err] = ln->Accept();
        EXPECT_TRUE(err);
        if (c) c->Close();
    }
}

// ---------------------------------------------------------------------------
// listen_test.go

TEST(GoNetDial, ListenConfigControl) {
    // StreamListen
    for (std::string network : {"tcp", "tcp4", "tcp6", "unix", "unixpacket"}) {
        if (!testableNetwork(network)) continue;
        std::string address = network.starts_with("unix") ? testUnixAddr()
                              : network == "tcp6"          ? std::string("[::1]:0")
                                                           : std::string("127.0.0.1:0");
        net::ListenConfig lc{.Control = controlOnConnSetup};
        auto [ln, err] = lc.Listen(context::Background(), network, address);
        EXPECT_FALSE(err) << network << ": " << err;
        if (ln) ln->Close();
    }
    // PacketListen
    for (std::string network : {"udp", "udp4", "udp6", "unixgram"}) {
        if (!testableNetwork(network)) continue;
        std::string address = network == "unixgram" ? testUnixAddr()
                              : network == "udp6"   ? std::string("[::1]:0")
                                                    : std::string("127.0.0.1:0");
        net::ListenConfig lc{.Control = controlOnConnSetup};
        auto [c, err] = lc.ListenPacket(context::Background(), network, address);
        EXPECT_FALSE(err) << network << ": " << err;
        if (c) c->Close();
        if (network == "unixgram") os::Remove(address);
    }
}

// ---------------------------------------------------------------------------
// tcpconn_keepalive_test.go

namespace {

struct keepAliveOpts {
    int enable = 0, idle = 0, interval = 0, count = 0;
};

keepAliveOpts readKeepAlive(net::TCPConn& c) {
    keepAliveOpts o;
    auto [rc, err] = c.SyscallConn();
    EXPECT_FALSE(err);
    rc->Control([&](uintptr s) {
        socklen_t len = sizeof(int);
        ::getsockopt(int(s), SOL_SOCKET, SO_KEEPALIVE, &o.enable, &len);
        ::getsockopt(int(s), IPPROTO_TCP, TCP_KEEPIDLE, &o.idle, &len);
        ::getsockopt(int(s), IPPROTO_TCP, TCP_KEEPINTVL, &o.interval, &len);
        ::getsockopt(int(s), IPPROTO_TCP, TCP_KEEPCNT, &o.count, &len);
    });
    return o;
}

// The configurations of Go's testConfigs, and the options they make.
struct keepAliveCase {
    net::KeepAliveConfig cfg;
    keepAliveOpts want;
};

std::vector<keepAliveCase> keepAliveCases() {
    return {
        {{.Enable = true, .Idle = 5 * time::Second, .Interval = 3 * time::Second, .Count = 10}, {1, 5, 3, 10}},
        {{.Enable = true, .Idle = time::Duration(0), .Interval = time::Duration(0), .Count = 0}, {1, 15, 15, 9}},
        {{.Enable = true, .Idle = 2 * time::Second, .Interval = 1 * time::Second, .Count = 3}, {1, 2, 1, 3}},
    };
}

}  // namespace

TEST(GoNetDial, TCPConnKeepAliveConfigDialer) {
    for (auto& tc : keepAliveCases()) {
        auto ln = newLocalListener("tcp");
        go([ln] {
            auto [c, err] = ln->Accept();
            if (!err) c->Close();
        });
        net::Dialer d{.KeepAlive = -time::Second, .KeepAliveConfig = tc.cfg};
        auto [c, err] = d.Dial("tcp", ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        auto o = readKeepAlive(dynamic_cast<net::TCPConn&>(*c));
        EXPECT_EQ(o.enable, tc.want.enable);
        EXPECT_EQ(o.idle, tc.want.idle);
        EXPECT_EQ(o.interval, tc.want.interval);
        EXPECT_EQ(o.count, tc.want.count);
        c->Close();
        ln->Close();
    }
}

TEST(GoNetDial, TCPConnKeepAliveConfigListener) {
    for (auto& tc : keepAliveCases()) {
        net::ListenConfig lc{.KeepAlive = -time::Second, .KeepAliveConfig = tc.cfg};
        auto [ln, err] = lc.Listen(context::Background(), "tcp", "127.0.0.1:0");
        ASSERT_FALSE(err) << err;
        auto [c, derr] = net::Dial("tcp", ln->Addr()->String().view());
        ASSERT_FALSE(derr) << derr;
        auto [sc, aerr] = ln->Accept();
        ASSERT_FALSE(aerr) << aerr;
        auto o = readKeepAlive(dynamic_cast<net::TCPConn&>(*sc));
        EXPECT_EQ(o.enable, tc.want.enable);
        EXPECT_EQ(o.idle, tc.want.idle);
        EXPECT_EQ(o.interval, tc.want.interval);
        EXPECT_EQ(o.count, tc.want.count);
        sc->Close();
        c->Close();
        ln->Close();
    }
}

TEST(GoNetDial, TCPConnKeepAliveConfig) {
    for (auto& tc : keepAliveCases()) {
        auto ln = newLocalListener("tcp");
        go([ln] {
            auto [c, err] = ln->Accept();
            if (!err) c->Close();
        });
        net::Dialer d{.KeepAlive = -time::Second};  // prevent calling hook from dialing
        auto [c, err] = d.Dial("tcp", ln->Addr()->String().view());
        ASSERT_FALSE(err) << err;
        auto& tc_ = dynamic_cast<net::TCPConn&>(*c);
        ASSERT_FALSE(tc_.SetKeepAliveConfig(tc.cfg));
        auto o = readKeepAlive(tc_);
        EXPECT_EQ(o.enable, tc.want.enable);
        EXPECT_EQ(o.idle, tc.want.idle);
        EXPECT_EQ(o.interval, tc.want.interval);
        EXPECT_EQ(o.count, tc.want.count);
        c->Close();
        ln->Close();
    }
}

// ---------------------------------------------------------------------------
// sendfile_test.go and splice_linux_test.go

namespace {

// A file of n bytes of a pattern, and its path.
std::pair<ptr<os::File>, std::string> patternFile(std::size_t n) {
    auto [f, err] = os::CreateTemp("", "gopp-sendfile");
    EXPECT_FALSE(err) << err;
    std::vector<byte> data(n);
    for (std::size_t i = 0; i < n; i++) data[i] = byte(i * 7 + 3);
    f->Write(data);
    f->Seek(0, io::SeekStart);
    return {f, std::string(f->Name().view())};
}

bool isPattern(std::span<const byte> b, std::size_t from) {
    for (std::size_t i = 0; i < b.size(); i++) {
        if (b[i] != byte((i + from) * 7 + 3)) return false;
    }
    return true;
}

// io::Copy(c, r) on the dialed side; the bytes the other side read.
slice<byte> copyAndRead(const std::function<void(net::TCPConn&)>& send) {
    auto ln = newLocalListener("tcp");
    auto got = make_chan<slice<byte>>(1);
    go([ln, got] {
        auto [c, err] = ln->Accept();
        if (err) {
            got.send({});
            return;
        }
        auto [b, rerr] = io::ReadAll(*c);
        EXPECT_FALSE(rerr) << rerr;
        c->Close();
        got.send(b);
    });
    auto [c, err] = net::Dial("tcp", ln->Addr()->String().view());
    EXPECT_FALSE(err) << err;
    send(dynamic_cast<net::TCPConn&>(*c));
    c->Close();
    auto b = got.recv();
    ln->Close();
    return b;
}

}  // namespace

TEST(GoNetDial, Sendfile) {
    constexpr std::size_t size = 1 << 20;
    auto [f, path] = patternFile(size);
    auto b = copyAndRead([&](net::TCPConn& c) {
        auto [n, err] = io::Copy(c, *f);
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(n, int64(size));
    });
    EXPECT_EQ(b.size(), size);
    EXPECT_TRUE(isPattern(b, 0));
    f->Close();
    os::Remove(path);
}

TEST(GoNetDial, SendfileParts) {
    // A LimitedReader sends part of the file, and the rest follows.
    constexpr std::size_t size = 64 << 10;
    auto [f, path] = patternFile(size);
    auto b = copyAndRead([&, f = f](net::TCPConn& c) {
        io::LimitedReader lr(f, 1000);
        auto [n, err] = c.ReadFrom(lr);
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(n, 1000);
        EXPECT_EQ(lr.N, 0);
        auto [n2, err2] = io::Copy(c, *f);
        EXPECT_FALSE(err2) << err2;
        EXPECT_EQ(n2, int64(size - 1000));
    });
    EXPECT_EQ(b.size(), size);
    EXPECT_TRUE(isPattern(b, 0));
    f->Close();
    os::Remove(path);
}

TEST(GoNetDial, SendfileSeeked) {
    constexpr std::size_t size = 64 << 10, seekTo = 8 << 10;
    auto [f, path] = patternFile(size);
    f->Seek(int64(seekTo), io::SeekStart);
    auto b = copyAndRead([&](net::TCPConn& c) {
        auto [n, err] = io::Copy(c, *f);
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(n, int64(size - seekTo));
    });
    EXPECT_EQ(b.size(), size - seekTo);
    EXPECT_TRUE(isPattern(b, seekTo));
    f->Close();
    os::Remove(path);
}

TEST(GoNetDial, SendfilePipe) {
    // A pipe isn't a regular file: the bytes go through the copy loop.
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err);
    go([w] {
        io::WriteString(*w, "from a pipe");
        w->Close();
    });
    auto b = copyAndRead([&, r = r](net::TCPConn& c) {
        auto [n, err] = io::Copy(c, *r);
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(n, 11);
    });
    EXPECT_EQ(asStringView(b), "from a pipe");
    r->Close();
}

TEST(GoNetDial, Splice) {
    // From a TCP connection and from a Unix one into TCP, with and without
    // a limit; and from TCP into a Unix connection.
    constexpr std::size_t size = 1 << 20;
    std::vector<byte> data(size);
    for (std::size_t i = 0; i < size; i++) data[i] = byte(i * 7 + 3);
    for (std::string srcNet : {"tcp", "unix"}) {
        for (int64 limit : {int64(-1), int64(size / 2)}) {
            SCOPED_TRACE(srcNet + " limit " + std::to_string(limit));
            // The source: a connection the other end writes data to.
            auto sln = newLocalListener(srcNet);
            go([sln, &data] {
                auto [c, err] = sln->Accept();
                if (err) return;
                c->Write(data);
                c->Close();
            });
            auto [src, err] = net::Dial(srcNet, sln->Addr()->String().view());
            ASSERT_FALSE(err) << err;
            int64 want = limit < 0 ? int64(size) : limit;
            auto b = copyAndRead([&](net::TCPConn& c) {
                int64 n;
                error err;
                if (limit < 0) {
                    std::tie(n, err) = io::Copy(c, *src);
                } else {
                    io::LimitedReader lr(src, limit);
                    std::tie(n, err) = c.ReadFrom(lr);
                }
                EXPECT_FALSE(err) << err;
                EXPECT_EQ(n, want);
            });
            EXPECT_EQ(int64(b.size()), want);
            EXPECT_TRUE(isPattern(b, 0));
            src->Close();
            sln->Close();
        }
    }
    // TCPConn.WriteTo a Unix connection.
    auto uln = newLocalListener("unix");
    auto got = make_chan<slice<byte>>(1);
    go([uln, got] {
        auto [c, err] = uln->Accept();
        if (err) {
            got.send({});
            return;
        }
        auto [b, rerr] = io::ReadAll(*c);
        c->Close();
        got.send(b);
    });
    auto [uc, uerr] = net::Dial("unix", uln->Addr()->String().view());
    ASSERT_FALSE(uerr);
    withTCPConnPair(
        [&](net::TCPConn& c) -> error {
            auto [n, err] = c.Write(data);
            return err;
        },
        [&](net::TCPConn& c) -> error {
            auto [n, err] = c.WriteTo(*uc);
            if (n != int64(size)) return errors::New("WriteTo wrote " + std::to_string(n));
            return err;
        });
    uc->Close();
    auto b = got.recv();
    EXPECT_EQ(b.size(), size);
    EXPECT_TRUE(isPattern(b, 0));
    uln->Close();
}

// ---------------------------------------------------------------------------
// mptcpsock_linux_test.go

TEST(GoNetDial, MultiPathTCP) {
    if (!net::supportsMultipathTCP()) GTEST_SKIP() << "MPTCP isn't supported";
    for (bool useMPTCP : {false, true}) {
        SCOPED_TRACE(useMPTCP ? "MPTCP" : "TCP");
        net::ListenConfig lc;
        lc.SetMultipathTCP(useMPTCP);
        EXPECT_EQ(lc.MultipathTCP(), useMPTCP);
        auto [ln, err] = lc.Listen(context::Background(), "tcp", "127.0.0.1:0");
        ASSERT_FALSE(err) << err;
        auto accepted = make_chan<ptr<net::Conn>>(1);
        go([ln, accepted] { accepted.send(ln->Accept().first); });
        net::Dialer d;
        d.SetMultipathTCP(useMPTCP);
        EXPECT_EQ(d.MultipathTCP(), useMPTCP);
        auto [c, derr] = d.Dial("tcp", ln->Addr()->String().view());
        ASSERT_FALSE(derr) << derr;
        auto sc = accepted.recv();
        ASSERT_TRUE(sc);
        auto [using_, merr] = dynamic_cast<net::TCPConn&>(*c).MultipathTCP();
        EXPECT_FALSE(merr);
        EXPECT_EQ(using_, useMPTCP);
        auto [susing, serr] = dynamic_cast<net::TCPConn&>(*sc).MultipathTCP();
        EXPECT_FALSE(serr);
        EXPECT_EQ(susing, useMPTCP);
        // Data goes through either way.
        io::WriteString(*c, "x");
        byte b[1];
        auto [n, rerr] = io::ReadFull(*sc, b);
        EXPECT_FALSE(rerr);
        sc->Close();
        c->Close();
        ln->Close();
    }
}

// ---------------------------------------------------------------------------
// udpsock_test.go: the AddrPort methods

TEST(GoNetDial, UDPConnAddrPortMethods) {
    auto [la, err] = net::ResolveUDPAddr("udp4", "127.0.0.1:0");
    ASSERT_FALSE(err);
    auto [c, lerr] = net::ListenUDP("udp4", la);
    ASSERT_FALSE(lerr) << lerr;
    auto self = dynamic_cast<net::UDPAddr*>(c->LocalAddr().get())->AddrPort();
    EXPECT_TRUE(self.IsValid());
    EXPECT_EQ(str(self.Addr().String()), "127.0.0.1");

    std::string wb = "UDPCONN TEST";
    auto [n, werr] = c->WriteToUDPAddrPort(as_bytes(wb), self);
    ASSERT_FALSE(werr) << werr;
    EXPECT_EQ(n, isize(wb.size()));
    byte rb[128];
    auto [rn, from, rerr] = c->ReadFromUDPAddrPort(rb);
    ASSERT_FALSE(rerr) << rerr;
    EXPECT_EQ(std::string_view(reinterpret_cast<char*>(rb), std::size_t(rn)), wb);
    EXPECT_EQ(from, self);

    auto [mn, moobn, merr] = c->WriteMsgUDPAddrPort(as_bytes(wb), {}, self);
    ASSERT_FALSE(merr) << merr;
    EXPECT_EQ(mn, isize(wb.size()));
    auto [rn2, roobn, flags, from2, rerr2] = c->ReadMsgUDPAddrPort(rb, {});
    ASSERT_FALSE(rerr2) << rerr2;
    EXPECT_EQ(rn2, isize(wb.size()));
    EXPECT_EQ(from2, self);

    // Not to a connected socket, and not without an address.
    EXPECT_TRUE(c->WriteToUDPAddrPort(as_bytes(wb), netip::AddrPort{}).second);
    auto ua = net::UDPAddrFromAddrPort(self);
    EXPECT_EQ(ua->String(), c->LocalAddr()->String());
    EXPECT_EQ(net::TCPAddrFromAddrPort(netip::MustParseAddrPort("[::1%lo]:80"))->String(), "[::1%lo]:80");
    EXPECT_FALSE(net::TCPAddr().AddrPort().IsValid());
    c->Close();
}
