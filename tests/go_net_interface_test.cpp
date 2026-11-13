// Ported from Go 1.27.3's net/interface_test.go, interface_linux_test.go
// (TestParseProcNet), mac_test.go and listen_test.go (the multicast
// listeners), with Go's test names.
//
// Left out: the tests that change interfaces (they need root), and the
// interface statistics' cross-checks against the routing table.
#include <gopp/net.hpp>
#include <gopp/os.hpp>

#include "go_net_testdata.hpp"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::net;
using namespace std::chrono_literals;

namespace gopp::net {
std::vector<ptr<Addr>> parseProcNetIGMPForTest(const std::string& path, const Interface* ifi);
std::vector<ptr<Addr>> parseProcNetIGMP6ForTest(const std::string& path, const Interface* ifi);
}  // namespace gopp::net

namespace {

std::string str(const string& s) { return std::string(s.view()); }

std::string bytesStr(std::span<const byte> b) { return std::string(as_string_view(b)); }

// interface_test.go's loopbackInterface.
ptr<Interface> loopbackInterface() {
    auto [ift, err] = Interfaces();
    if (err) return nullptr;
    for (auto& ifi : ift) {
        if ((ifi.Flags & FlagLoopback) && (ifi.Flags & FlagUp)) return make_ptr<Interface>(ifi);
    }
    return nullptr;
}

bool interfaceEqual(const Interface& a, const Interface& b) {
    return a.Index == b.Index && a.MTU == b.MTU && a.Name == b.Name &&
           bytesStr(a.HardwareAddr) == bytesStr(b.HardwareAddr) && unsigned(a.Flags) == unsigned(b.Flags);
}

struct routeStats {
    int ipv4 = 0, ipv6 = 0;
};

// interface_test.go's validateInterfaceUnicastAddrs.
routeStats validateInterfaceUnicastAddrs(const std::vector<ptr<Addr>>& ifat) {
    routeStats stats;
    for (auto& a : ifat) {
        auto* ifa = dynamic_cast<IPNet*>(a.get());
        EXPECT_TRUE(ifa) << "unexpected type: " << str(a->Network());
        if (!ifa) continue;
        EXPECT_FALSE(ifa->IP.empty() || ifa->IP.IsMulticast() || ifa->Mask.empty()) << str(ifa->String());
        EXPECT_EQ(ifa->IP.size(), std::size_t(IPv6len))
            << "should be internal representation either IPv6 or IPv4-mapped IPv6 address: " << str(ifa->String());
        auto [prefixLen, maxPrefixLen] = ifa->Mask.Size();
        if (!ifa->IP.To4().empty()) {
            EXPECT_TRUE(0 < prefixLen && prefixLen <= 8 * IPv4len && maxPrefixLen == 8 * IPv4len) << str(ifa->String());
            if (ifa->IP.IsLoopback()) {
                EXPECT_GE(prefixLen, 8) << str(ifa->String());  // see RFC 1122
            }
            stats.ipv4++;
        }
        if (!ifa->IP.To16().empty() && ifa->IP.To4().empty()) {
            EXPECT_TRUE(0 < prefixLen && prefixLen <= 8 * IPv6len && maxPrefixLen == 8 * IPv6len) << str(ifa->String());
            if (ifa->IP.IsLoopback()) {
                EXPECT_EQ(prefixLen, 8 * IPv6len) << str(ifa->String());  // see RFC 4291
            }
            stats.ipv6++;
        }
    }
    return stats;
}

// listen_test.go's multicastRIBContains.
bool multicastRIBContains(const IP& ip) {
    auto [ift, err] = Interfaces();
    EXPECT_FALSE(err);
    for (auto& ifi : ift) {
        auto [ifmat, merr] = ifi.MulticastAddrs();
        EXPECT_FALSE(merr);
        for (auto& ifma : ifmat) {
            if (static_cast<IPAddr*>(ifma.get())->IP.Equal(ip)) return true;
        }
    }
    return false;
}

void checkMulticastListener(const ptr<UDPConn>& c, const IP& ip) {
    EXPECT_TRUE(multicastRIBContains(ip)) << str(ip.String()) << " not found in multicast rib";
    auto* la = dynamic_cast<UDPAddr*>(c->LocalAddr().get());
    EXPECT_TRUE(la && la->Port != 0) << "got " << (la ? str(la->String()) : "nil")
                                     << "; want a proper address with non-zero port number";
}

}  // namespace

// ---------------------------------------------------------------------------
// mac_test.go

TEST(GoNetInterface, ParseMAC) {
    struct {
        std::string in;
        std::vector<byte> out;
        std::string err;
    } tests[] = {
        // See RFC 7042, Section 2.1.1.
        {"00:00:5e:00:53:01", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, ""},
        {"00-00-5e-00-53-01", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, ""},
        {"0000.5e00.5301", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, ""},
        {"00005e005301", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, ""},
        // See RFC 7042, Section 2.2.2.
        {"02:00:5e:10:00:00:00:01", {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01}, ""},
        {"02-00-5e-10-00-00-00-01", {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01}, ""},
        {"0200.5e10.0000.0001", {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01}, ""},
        {"02005e1000000001", {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01}, ""},
        // See RFC 4391, Section 9.1.1.
        {"00:00:00:00:fe:80:00:00:00:00:00:00:02:00:5e:10:00:00:00:01",
         {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00,
          0x00, 0x01},
         ""},
        {"00-00-00-00-fe-80-00-00-00-00-00-00-02-00-5e-10-00-00-00-01",
         {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00,
          0x00, 0x01},
         ""},
        {"0000.0000.fe80.0000.0000.0000.0200.5e10.0000.0001",
         {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00,
          0x00, 0x01},
         ""},
        {"00000000fe8000000000000002005e1000000001",
         {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00,
          0x00, 0x01},
         ""},
        {"ab:cd:ef:AB:CD:EF", {0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef}, ""},
        {"ab:cd:ef:AB:CD:EF:ab:cd", {0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd}, ""},
        {"ab:cd:ef:AB:CD:EF:ab:cd:ef:AB:CD:EF:ab:cd:ef:AB:CD:EF:ab:cd",
         {0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef,
          0xab, 0xcd},
         ""},
        {"01.02.03.04.05.06", {}, "invalid MAC address"},
        {"01:02:03:04:05:06:", {}, "invalid MAC address"},
        {"x1:02:03:04:05:06", {}, "invalid MAC address"},
        {"01002:03:04:05:06", {}, "invalid MAC address"},
        {"01:02003:04:05:06", {}, "invalid MAC address"},
        {"01:02:03004:05:06", {}, "invalid MAC address"},
        {"01:02:03:04005:06", {}, "invalid MAC address"},
        {"01:02:03:04:05006", {}, "invalid MAC address"},
        {"01-02:03:04:05:06", {}, "invalid MAC address"},
        {"01:02-03-04-05-06", {}, "invalid MAC address"},
        {"0123:4567:89AF", {}, "invalid MAC address"},
        {"0123-4567-89AF", {}, "invalid MAC address"},
        {"0123456789AF0", {}, "invalid MAC address"},
    };
    int i = 0;
    for (auto& tt : tests) {
        auto [out, err] = ParseMAC(tt.in);
        EXPECT_EQ(bytesStr(out), bytesStr(tt.out)) << tt.in;
        if (tt.err.empty()) {
            EXPECT_FALSE(err) << tt.in << ": " << err;
            // Serialization works too, and round-trips.
            auto [out2, err2] = ParseMAC(out.String().view());
            ASSERT_FALSE(err2) << i << ". ParseMAC(" << str(out.String()) << ")";
            EXPECT_EQ(bytesStr(out2), bytesStr(out)) << i;
        } else {
            ASSERT_TRUE(err) << tt.in;
            EXPECT_NE(str(err.Error()).find(tt.err), std::string::npos) << err;
        }
        i++;
    }
}

// ---------------------------------------------------------------------------
// interface_linux_test.go

TEST(GoNetInterface, ParseProcNet) {
    auto [dir, err] = os::MkdirTemp("", "gopp-procnet");
    ASSERT_FALSE(err);
    std::string igmp = std::string(dir.view()) + "/igmp", igmp6 = std::string(dir.view()) + "/igmp6";
    os::WriteFile(igmp, std::string_view(go_net_testdata::files().at("igmp")), 0644);
    os::WriteFile(igmp6, std::string_view(go_net_testdata::files().at("igmp6")), 0644);
    auto iface = [](std::string name) {
        Interface i;
        i.Name = string(name);
        return i;
    };
    std::vector<std::string> igmpInterfaceTable = {"lo",       "eth0",     "eth1",     "eth2",       "eth0.100",
                                                   "eth0.101", "eth0.102", "eth0.103", "device1tap2"};
    std::size_t n4 = 0, n6 = 0;
    for (auto& name : igmpInterfaceTable) {
        Interface ifi = iface(name);
        n4 += parseProcNetIGMPForTest(igmp, &ifi).size();
    }
    EXPECT_EQ(n4, 14u);  // numOfTestIPv4MCAddrs
    auto igmp6InterfaceTable = igmpInterfaceTable;
    igmp6InterfaceTable.push_back("pan0");
    for (auto& name : igmp6InterfaceTable) {
        Interface ifi = iface(name);
        n6 += parseProcNetIGMP6ForTest(igmp6, &ifi).size();
    }
    EXPECT_EQ(n6, 18u);  // numOfTestIPv6MCAddrs
    os::RemoveAll(dir.view());
}

// ---------------------------------------------------------------------------
// interface_test.go

TEST(GoNetInterface, Interfaces) {
    auto [ift, err] = Interfaces();
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(ift.empty());
    for (auto& ifi : ift) {
        auto [ifxi, xerr] = InterfaceByIndex(ifi.Index);
        ASSERT_FALSE(xerr) << xerr;
        EXPECT_TRUE(interfaceEqual(*ifxi, ifi)) << str(ifi.Name);
        if (!ifi.Name.empty()) {
            auto [ifxn, nerr] = InterfaceByName(ifi.Name.view());
            ASSERT_FALSE(nerr) << nerr;
            EXPECT_TRUE(interfaceEqual(*ifxn, ifi)) << str(ifi.Name);
        }
    }
}

TEST(GoNetInterface, InterfaceAddrs) {
    auto [ifat, err] = InterfaceAddrs();
    ASSERT_FALSE(err) << err;
    auto stats = validateInterfaceUnicastAddrs(ifat);
    // A loopback interface: 127.0.0.1/8, and ::1/128 with IPv6.
    if (loopbackInterface()) { EXPECT_GT(stats.ipv4, 0); }
}

TEST(GoNetInterface, InterfaceUnicastAddrs) {
    auto [ift, err] = Interfaces();
    ASSERT_FALSE(err) << err;
    routeStats uniStats;
    for (auto& ifi : ift) {
        auto [ifat, aerr] = ifi.Addrs();
        ASSERT_FALSE(aerr) << str(ifi.Name) << ": " << aerr;
        auto stats = validateInterfaceUnicastAddrs(ifat);
        uniStats.ipv4 += stats.ipv4;
        uniStats.ipv6 += stats.ipv6;
    }
    auto [all, aerr] = InterfaceAddrs();
    ASSERT_FALSE(aerr);
    auto allStats = validateInterfaceUnicastAddrs(all);
    EXPECT_EQ(uniStats.ipv4, allStats.ipv4);
    EXPECT_EQ(uniStats.ipv6, allStats.ipv6);
}

TEST(GoNetInterface, InterfaceMulticastAddrs) {
    auto [ift, err] = Interfaces();
    ASSERT_FALSE(err) << err;
    for (auto& ifi : ift) {
        auto [ifmat, merr] = ifi.MulticastAddrs();
        ASSERT_FALSE(merr) << str(ifi.Name) << ": " << merr;
        for (auto& ifma : ifmat) {
            auto* a = dynamic_cast<IPAddr*>(ifma.get());
            ASSERT_TRUE(a) << "unexpected type";
            EXPECT_TRUE(a->IP.IsMulticast()) << str(a->String());
        }
    }
}

TEST(GoNetInterface, InterfaceByErrors) {
    EXPECT_TRUE(InterfaceByIndex(0).second);
    EXPECT_TRUE(InterfaceByIndex(-1).second);
    EXPECT_TRUE(InterfaceByName("").second);
    EXPECT_TRUE(InterfaceByName("no-such-interface-0").second);
}

TEST(GoNetInterface, FlagsString) {
    EXPECT_EQ(str(Flags(0).String()), "0");
    EXPECT_EQ(str(Flags(FlagUp | FlagLoopback | FlagRunning).String()), "up|loopback|running");
    EXPECT_EQ(str(Flags(FlagUp | FlagBroadcast | FlagMulticast).String()), "up|broadcast|multicast");
    EXPECT_EQ(str(HardwareAddr{}.String()), "");
}

// ---------------------------------------------------------------------------
// listen_test.go

TEST(GoNetInterface, IPv4MulticastListener) {
    auto ifi = loopbackInterface();
    if (!ifi) GTEST_SKIP() << "no loopback interface";
    for (std::string net : {"udp", "udp4"}) {
        auto gaddr = make_ptr<UDPAddr>(IPv4(224, 0, 0, 254), 12345);  // see RFC 4727
        auto [c0, err] = ListenMulticastUDP(net, ifi, gaddr);
        ASSERT_FALSE(err) << err;
        checkMulticastListener(c0, gaddr->IP);
        auto [c1, err1] = ListenMulticastUDP(net, ifi, gaddr);
        ASSERT_FALSE(err1) << err1;
        checkMulticastListener(c1, gaddr->IP);
        c0->Close();
        c1->Close();
    }
}

TEST(GoNetInterface, IPv6MulticastListener) {
    auto ifi = loopbackInterface();
    if (!ifi) GTEST_SKIP() << "no loopback interface";
    if (!ParseIP("::1").To16().size() || Listen("tcp6", "[::1]:0").second) GTEST_SKIP() << "IPv6 is not supported";
    if (!(ifi->Flags & FlagMulticast)) GTEST_SKIP() << "the loopback interface can't do IPv6 multicast";
    for (std::string net : {"udp", "udp6"}) {
        for (std::string g : {"ff01::114", "ff02::114", "ff04::114", "ff05::114", "ff08::114", "ff0e::114"}) {
            auto gaddr = make_ptr<UDPAddr>(ParseIP(g), 12345);
            auto [c0, err] = ListenMulticastUDP(net, ifi, gaddr);
            ASSERT_FALSE(err) << g << ": " << err;
            checkMulticastListener(c0, gaddr->IP);
            auto [c1, err1] = ListenMulticastUDP(net, ifi, gaddr);
            ASSERT_FALSE(err1) << err1;
            checkMulticastListener(c1, gaddr->IP);
            c0->Close();
            c1->Close();
        }
    }
}

// go++'s own: packets sent to the group arrive at both listeners.
TEST(GoNetInterface, MulticastRoundTrip) {
    auto ifi = loopbackInterface();
    if (!ifi) GTEST_SKIP() << "no loopback interface";
    auto gaddr = make_ptr<UDPAddr>(IPv4(224, 0, 0, 254), 12346);
    auto [c0, err] = ListenMulticastUDP("udp4", ifi, gaddr);
    ASSERT_FALSE(err) << err;
    auto [c1, err1] = ListenMulticastUDP("udp4", ifi, gaddr);
    ASSERT_FALSE(err1) << err1;
    // A sender with loopback on, through the loopback interface.
    auto [s, serr] = ListenUDP("udp4", make_ptr<UDPAddr>(IPv4(127, 0, 0, 1), 0));
    ASSERT_FALSE(serr);
    auto [rc, rerr] = s->SyscallConn();
    ASSERT_FALSE(rerr);
    rc->Control([&](uintptr fd) {
        ip_mreqn mreq{};
        mreq.imr_ifindex = ifi->Index;
        ::setsockopt(int(fd), IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq));
        int on = 1;
        ::setsockopt(int(fd), IPPROTO_IP, IP_MULTICAST_LOOP, &on, sizeof(on));
    });
    auto [n, werr] = s->WriteToUDP(as_bytes("to the group"), gaddr);
    ASSERT_FALSE(werr) << werr;
    for (auto& c : {c0, c1}) {
        c->SetReadDeadline(time::Now().Add(2 * time::Second));
        byte b[64];
        auto [rn, from, rerr2] = c->ReadFromUDP(b);
        ASSERT_FALSE(rerr2) << rerr2;
        EXPECT_EQ(std::string_view(reinterpret_cast<char*>(b), std::size_t(rn)), "to the group");
    }
    s->Close();
    c0->Close();
    c1->Close();
}
