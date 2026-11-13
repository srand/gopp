// Ported from Go 1.27.3's net/iprawsock_test.go, with Go's test names and
// tables, and an ICMP echo through raw sockets of go++'s own. Raw sockets
// need CAP_NET_RAW: without it, the tests that open one are skipped, as
// Go's are.
#include <gopp/net.hpp>

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::net;

namespace {

std::string str(const string& s) { return std::string(s.view()); }

bool ipAddrEqual(const ptr<IPAddr>& a, const ptr<IPAddr>& b) {
    if (!a || !b) return !a && !b;
    return as_string_view(a->IP) == as_string_view(b->IP) && a->Zone == b->Zone;
}

// Whether raw IP sockets can be opened (Go's testableNetwork("ip")).
bool canRaw() {
    static bool ok = [] {
        auto [c, err] = ListenIP("ip4:icmp", make_ptr<IPAddr>(IPv4(127, 0, 0, 1)));
        if (err) return false;
        c->Close();
        return true;
    }();
    return ok;
}

bool notPermitted(const error& err) {
    auto* e = errors::As<Errno>(err);
    return e && (e->Code == EPERM || e->Code == EACCES || e->Code == EPROTONOSUPPORT);
}

}  // namespace

TEST(GoNetIPRaw, ResolveIPAddr) {
    enum errKind { none, unknownNetwork, noSuitable };
    struct {
        std::string network, litAddrOrName;
        ptr<IPAddr> addr;
        errKind err;
        std::string errArg;
    } tests[] = {
        {"ip", "127.0.0.1", make_ptr<IPAddr>(IPv4(127, 0, 0, 1)), none, ""},
        {"ip4", "127.0.0.1", make_ptr<IPAddr>(IPv4(127, 0, 0, 1)), none, ""},
        {"ip4:icmp", "127.0.0.1", make_ptr<IPAddr>(IPv4(127, 0, 0, 1)), none, ""},
        {"ip", "::1", make_ptr<IPAddr>(ParseIP("::1")), none, ""},
        {"ip6", "::1", make_ptr<IPAddr>(ParseIP("::1")), none, ""},
        {"ip6:ipv6-icmp", "::1", make_ptr<IPAddr>(ParseIP("::1")), none, ""},
        {"ip6:IPv6-ICMP", "::1", make_ptr<IPAddr>(ParseIP("::1")), none, ""},
        {"ip", "::1%en0", make_ptr<IPAddr>(ParseIP("::1"), "en0"), none, ""},
        {"ip6", "::1%911", make_ptr<IPAddr>(ParseIP("::1"), "911"), none, ""},
        {"", "127.0.0.1", make_ptr<IPAddr>(IPv4(127, 0, 0, 1)), none, ""},  // Go 1.0 behavior
        {"", "::1", make_ptr<IPAddr>(ParseIP("::1")), none, ""},            // Go 1.0 behavior
        {"ip4:icmp", "", make_ptr<IPAddr>(), none, ""},
        {"l2tp", "127.0.0.1", nullptr, unknownNetwork, "l2tp"},
        {"l2tp:gre", "127.0.0.1", nullptr, unknownNetwork, "l2tp:gre"},
        {"tcp", "1.2.3.4:123", nullptr, unknownNetwork, "tcp"},
        {"ip4", "2001:db8::1", nullptr, noSuitable, "2001:db8::1"},
        {"ip4:icmp", "2001:db8::1", nullptr, noSuitable, "2001:db8::1"},
        {"ip6", "127.0.0.1", nullptr, noSuitable, "127.0.0.1"},
        {"ip6", "::ffff:127.0.0.1", nullptr, noSuitable, "::ffff:127.0.0.1"},
        {"ip6:ipv6-icmp", "127.0.0.1", nullptr, noSuitable, "127.0.0.1"},
        {"ip6:ipv6-icmp", "::ffff:127.0.0.1", nullptr, noSuitable, "::ffff:127.0.0.1"},
    };
    for (auto& tt : tests) {
        SCOPED_TRACE(tt.network + " " + tt.litAddrOrName);
        auto [addr, err] = ResolveIPAddr(tt.network, tt.litAddrOrName);
        EXPECT_TRUE(ipAddrEqual(addr, tt.addr)) << (addr ? str(addr->String()) : "nil");
        switch (tt.err) {
        case none: EXPECT_FALSE(err) << err; break;
        case unknownNetwork: {
            auto* e = errors::As<UnknownNetworkError>(err);
            ASSERT_TRUE(e) << err;
            EXPECT_EQ(str(e->Net), tt.errArg);
            break;
        }
        case noSuitable: {
            auto* e = errors::As<AddrError>(err);
            ASSERT_TRUE(e) << err;
            EXPECT_EQ(str(e->Err), "no suitable address found");
            EXPECT_EQ(str(e->Addr), tt.errArg);
            break;
        }
        }
        if (!err) {
            auto [addr2, err2] = ResolveIPAddr(addr->Network().view(), addr->String().view());
            EXPECT_FALSE(err2) << err2;
            EXPECT_TRUE(ipAddrEqual(addr2, tt.addr));
        }
    }
}

TEST(GoNetIPRaw, IPConnLocalName) {
    if (!canRaw()) GTEST_SKIP() << "raw IP sockets need CAP_NET_RAW";
    for (auto laddr : {make_ptr<IPAddr>(IPv4(127, 0, 0, 1)), make_ptr<IPAddr>(), ptr<IPAddr>()}) {
        auto [c, err] = ListenIP("ip4:icmp", laddr);
        ASSERT_FALSE(err) << err;
        EXPECT_TRUE(c->LocalAddr()) << "should not fail";
        c->Close();
    }
}

TEST(GoNetIPRaw, IPConnRemoteName) {
    if (!canRaw()) GTEST_SKIP() << "raw IP sockets need CAP_NET_RAW";
    auto raddr = make_ptr<IPAddr>(IPv4(127, 0, 0, 1).To4());
    auto [c, err] = DialIP("ip:tcp", make_ptr<IPAddr>(IPv4(127, 0, 0, 1)), raddr);
    ASSERT_FALSE(err) << err;
    auto ra = c->RemoteAddr();
    ASSERT_TRUE(ra);
    EXPECT_EQ(str(ra->String()), "127.0.0.1");
    EXPECT_EQ(str(ra->Network()), "ip");
    c->Close();
}

TEST(GoNetIPRaw, DialListenIPArgs) {
    struct test {
        std::vector<std::pair<std::string, std::string>> argLists;
        bool shouldFail;
    };
    std::vector<test> tests = {
        {{{"ip", "127.0.0.1"},
          {"ip:", "127.0.0.1"},
          {"ip::", "127.0.0.1"},
          {"ip", "::1"},
          {"ip:", "::1"},
          {"ip::", "::1"},
          {"ip4", "127.0.0.1"},
          {"ip4:", "127.0.0.1"},
          {"ip4::", "127.0.0.1"},
          {"ip6", "::1"},
          {"ip6:", "::1"},
          {"ip6::", "::1"}},
         true},
    };
    if (canRaw()) {
        test priv{{}, false};
        for (auto [network, address] : std::vector<std::pair<std::string, std::string>>{{"ip4:47", "127.0.0.1"},
                                                                                          {"ip6:47", "::1"}}) {
            auto [c, err] = ListenPacket(network, address);
            if (err) continue;
            c->Close();
            priv.argLists.push_back({network, address});
        }
        if (!priv.argLists.empty()) tests.push_back(priv);
    }
    for (auto& tt : tests) {
        for (auto& [net, addr] : tt.argLists) {
            SCOPED_TRACE(net + " " + addr);
            auto [c1, err1] = Dial(net, addr);
            EXPECT_EQ(tt.shouldFail, bool(err1)) << err1;
            if (c1) c1->Close();
            auto [c2, err2] = ListenPacket(net, addr);
            EXPECT_EQ(tt.shouldFail, bool(err2)) << err2;
            if (c2) c2->Close();
            auto [a, aerr] = ResolveIPAddr("ip", addr);
            ASSERT_FALSE(aerr) << aerr;
            auto [c3, err3] = DialIP(net, nullptr, a);
            EXPECT_EQ(tt.shouldFail, bool(err3)) << err3;
            if (c3) c3->Close();
            auto [c4, err4] = ListenIP(net, a);
            EXPECT_EQ(tt.shouldFail, bool(err4)) << err4;
            if (c4) c4->Close();
        }
    }
}

// go++'s own: an ICMP echo request to 127.0.0.1 through a raw socket gets
// its reply, with the IP header removed.
TEST(GoNetIPRaw, ICMPEcho) {
    auto [c, err] = ListenPacket("ip4:icmp", "127.0.0.1");
    if (err && notPermitted(err)) GTEST_SKIP() << "raw IP sockets need CAP_NET_RAW: " << err;
    ASSERT_FALSE(err) << err;
    // Echo request: type 8, code 0, checksum, id, seq, data.
    std::vector<byte> msg = {8, 0, 0, 0, 0x12, 0x34, 0x00, 0x01, 'g', 'o', 'p', 'p'};
    uint32 sum = 0;
    for (std::size_t i = 0; i + 1 < msg.size(); i += 2) sum += uint32(msg[i] << 8 | msg[i + 1]);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16 cs = uint16(~sum);
    msg[2] = byte(cs >> 8);
    msg[3] = byte(cs);
    auto dst = make_ptr<IPAddr>(IPv4(127, 0, 0, 1));
    auto [n, werr] = c->WriteTo(msg, dst);
    ASSERT_FALSE(werr) << werr;
    c->SetReadDeadline(time::Now().Add(2 * time::Second));
    // Our own request comes back too (type 8); look for the reply (type 0).
    for (int i = 0; i < 4; i++) {
        byte b[1500];
        auto [rn, from, rerr] = c->ReadFrom(b);
        ASSERT_FALSE(rerr) << rerr;
        ASSERT_GE(rn, 12);
        if (b[0] != 0) continue;
        EXPECT_EQ(str(from->String()), "127.0.0.1");
        EXPECT_EQ(b[4], 0x12);
        EXPECT_EQ(b[5], 0x34);
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(b + 8), 4), "gopp");
        c->Close();
        return;
    }
    FAIL() << "no echo reply";
}
