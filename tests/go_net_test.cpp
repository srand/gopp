// Ported from Go 1.27.3's net/ip_test.go, ipsock_test.go (SplitHostPort,
// JoinHostPort), tcpsock_test.go (TestResolveTCPAddr), udpsock_test.go and
// packetconn_test.go, with Go's test names and tables.
//
// Left out: the netip variants (AddrPort, ReadFromUDPAddrPort and so on),
// as go++ has no net/netip; the IPv6 link-local and allocation tests, and
// benchmarks; TestLookupWithIP, which needs DNS.
#include <gopp/errors.hpp>
#include <gopp/net.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <ostream>
#include <string>
#include <vector>

using namespace gopp;
using namespace std::chrono_literals;

namespace gopp::net {

// For googletest's messages.
std::ostream& operator<<(std::ostream& os, const IP& ip) { return os << ip.String().view(); }

template <typename A>
    requires std::is_base_of_v<Addr, A>
std::ostream& operator<<(std::ostream& os, const ptr<A>& a) {
    return os << (a ? a->String().view() : std::string_view("<nil>"));
}

}  // namespace gopp::net
using net::IP;
using net::IPMask;

namespace {

std::string str(const string& s) { return std::string(s.view()); }

// reflect.DeepEqual of two byte slices: nil and empty differ only in Go,
// where both read as empty here.
bool deep_equal(std::span<const byte> a, std::span<const byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

template <typename A>
bool addr_equal(const ptr<A>& a, const ptr<A>& b) {
    if (!a || !b) return !a && !b;
    return deep_equal(a->IP, b->IP) && a->Port == b->Port && a->Zone == b->Zone;
}

// The error tables' errors, compared as reflect.DeepEqual would.
struct want_err {
    enum { none, unknown_network, addr_error } kind = none;
    std::string a, b;
};

want_err UnknownNetworkError(std::string net) { return {want_err::unknown_network, std::move(net), {}}; }
want_err AddrError(std::string err, std::string addr) { return {want_err::addr_error, std::move(err), std::move(addr)}; }

bool err_equal(const error& err, const want_err& w) {
    switch (w.kind) {
    case want_err::none: return !err;
    case want_err::unknown_network: {
        auto* e = errors::As<net::UnknownNetworkError>(err);
        return e && str(e->Net) == w.a;
    }
    case want_err::addr_error: {
        auto* e = errors::As<net::AddrError>(err);
        return e && str(e->Err) == w.a && str(e->Addr) == w.b;
    }
    }
    return false;
}

const std::string errNoSuitableAddress = "no suitable address found";

// mockserver.go's newLocalPacketListener for "udp".
ptr<net::PacketConn> newLocalPacketListener() {
    auto [c, err] = net::ListenPacket("udp", "127.0.0.1:0");
    EXPECT_FALSE(err) << err;
    return c;
}

std::span<const byte> b(std::string_view s) { return as_bytes(s); }

}  // namespace

// ---------------------------------------------------------------------------
// ip_test.go

TEST(GoNet, ParseIP) {
    struct {
        std::string in;
        IP out;
    } parseIPTests[] = {
        {"127.0.1.2", net::IPv4(127, 0, 1, 2)},
        {"127.0.0.1", net::IPv4(127, 0, 0, 1)},
        {"::ffff:127.1.2.3", net::IPv4(127, 1, 2, 3)},
        {"::ffff:7f01:0203", net::IPv4(127, 1, 2, 3)},
        {"0:0:0:0:0000:ffff:127.1.2.3", net::IPv4(127, 1, 2, 3)},
        {"0:0:0:0::ffff:127.1.2.3", net::IPv4(127, 1, 2, 3)},

        {"2001:4860:0:2001::68", IP{0x20, 0x01, 0x48, 0x60, 0, 0, 0x20, 0x01, 0, 0, 0, 0, 0, 0, 0x00, 0x68}},
        {"2001:4860:0000:2001:0000:0000:0000:0068",
         IP{0x20, 0x01, 0x48, 0x60, 0, 0, 0x20, 0x01, 0, 0, 0, 0, 0, 0, 0x00, 0x68}},

        {"-0.0.0.0", {}},
        {"0.-1.0.0", {}},
        {"0.0.-2.0", {}},
        {"0.0.0.-3", {}},
        {"127.0.0.256", {}},
        {"abc", {}},
        {"123:", {}},
        {"fe80::1%lo0", {}},
        {"fe80::1%911", {}},
        {"", {}},
        // 6 zeroes in one group
        {"0:0:0:0:000000:ffff:127.1.2.3", {}},
        // 5 zeroes in one group edge case
        {"0:0:0:0:00000:ffff:127.1.2.3", {}},
        {"a1:a2:a3:a4::b1:b2:b3:b4", {}},  // Issue 6628
        {"127.001.002.003", {}},
        {"::ffff:127.001.002.003", {}},
        {"123.000.000.000", {}},
        {"1.2..4", {}},
        {"0123.0.0.1", {}},
    };
    for (auto& tt : parseIPTests) {
        if (IP out = net::ParseIP(tt.in); !deep_equal(out, tt.out)) {
            ADD_FAILURE() << "ParseIP(" << tt.in << ") = " << out.String() << ", want " << tt.out.String();
        }
        if (tt.in.empty()) continue;  // Tested in TestMarshalEmptyIP below.
        IP out;
        error err = out.UnmarshalText(as_bytes(tt.in));
        if (!deep_equal(out, tt.out) || tt.out.empty() != static_cast<bool>(err)) {
            ADD_FAILURE() << "IP.UnmarshalText(" << tt.in << ") = " << out.String() << ", " << err << ", want "
                          << tt.out.String();
        }
    }
}

TEST(GoNet, MarshalEmptyIP) {
    for (std::string_view in : {std::string_view(), std::string_view("")}) {
        IP out{1, 2, 3, 4};
        if (error err = out.UnmarshalText(as_bytes(in)); err || !out.empty()) {
            ADD_FAILURE() << "UnmarshalText = " << out.String() << ", " << err << "; want nil, nil";
        }
    }
    IP ip;
    auto [got, err] = ip.MarshalText();
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(got.empty());

    auto buf = slice<byte>::make(4);
    auto [got2, err2] = ip.AppendText(buf);
    ASSERT_FALSE(err2) << err2;
    EXPECT_TRUE(deep_equal(got2, std::vector<byte>{0, 0, 0, 0}));
}

TEST(GoNet, IPString) {
    struct {
        IP in;
        std::string str;
        std::string byt;
        bool err;
    } ipStringTests[] = {
        // IPv4 address
        {IP{192, 0, 2, 1}, "192.0.2.1", "192.0.2.1", false},
        {IP{0, 0, 0, 0}, "0.0.0.0", "0.0.0.0", false},
        // IPv4-mapped IPv6 address
        {IP{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1}, "192.0.2.1", "192.0.2.1", false},
        {IP{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0}, "0.0.0.0", "0.0.0.0", false},
        // IPv6 address
        {IP{0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0x1, 0x23, 0, 0x12, 0, 0x1}, "2001:db8::123:12:1",
         "2001:db8::123:12:1", false},
        {IP{0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1}, "2001:db8::1", "2001:db8::1", false},
        {IP{0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0x1, 0, 0, 0, 0x1, 0, 0, 0, 0x1}, "2001:db8:0:1:0:1:0:1",
         "2001:db8:0:1:0:1:0:1", false},
        {IP{0x20, 0x1, 0xd, 0xb8, 0, 0x1, 0, 0, 0, 0x1, 0, 0, 0, 0x1, 0, 0}, "2001:db8:1:0:1:0:1:0",
         "2001:db8:1:0:1:0:1:0", false},
        {IP{0x20, 0x1, 0, 0, 0, 0, 0, 0, 0, 0x1, 0, 0, 0, 0, 0, 0x1}, "2001::1:0:0:1", "2001::1:0:0:1", false},
        {IP{0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0x1, 0, 0, 0, 0, 0, 0}, "2001:db8:0:0:1::", "2001:db8:0:0:1::",
         false},
        {IP{0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0x1, 0, 0, 0, 0, 0, 0x1}, "2001:db8::1:0:0:1", "2001:db8::1:0:0:1",
         false},
        {IP{0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0xa, 0, 0xb, 0, 0xc, 0, 0xd}, "2001:db8::a:b:c:d",
         "2001:db8::a:b:c:d", false},
        {net::IPv6unspecified, "::", "::", false},
        // IP wildcard equivalent address in Dial/Listen API
        {IP{}, "<nil>", "", false},
        // Opaque byte sequence
        {IP{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}, "?0123456789abcdef", "", true},
    };
    for (auto& tt : ipStringTests) {
        EXPECT_EQ(str(tt.in.String()), tt.str);
        auto [out, err] = tt.in.MarshalText();
        EXPECT_EQ(as_string_view(out), tt.byt) << tt.str;
        EXPECT_EQ(static_cast<bool>(err), tt.err) << tt.str;
        if (tt.err) {
            auto* e = errors::As<net::AddrError>(err);
            ASSERT_TRUE(e);
            EXPECT_EQ(str(e->Err), "invalid IP address");
            EXPECT_EQ(str(e->Addr), "0123456789abcdef");
        }
        auto buf = slice<byte>::make(4, 32);
        auto [out2, err2] = tt.in.AppendText(buf);
        EXPECT_EQ(as_string_view(out2.span().subspan(4)), tt.byt) << tt.str;
        EXPECT_EQ(static_cast<bool>(err2), tt.err) << tt.str;
    }
}

TEST(GoNet, IPMask) {
    struct {
        IP in;
        IPMask mask;
        IP out;
    } ipMaskTests[] = {
        {net::IPv4(192, 168, 1, 127), net::IPv4Mask(255, 255, 255, 128), net::IPv4(192, 168, 1, 0)},
        {net::IPv4(192, 168, 1, 127), IPMask(net::ParseIP("255.255.255.192")), net::IPv4(192, 168, 1, 64)},
        {net::IPv4(192, 168, 1, 127), IPMask(net::ParseIP("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffe0")),
         net::IPv4(192, 168, 1, 96)},
        {net::IPv4(192, 168, 1, 127), net::IPv4Mask(255, 0, 255, 0), net::IPv4(192, 0, 1, 0)},
        {net::ParseIP("2001:db8::1"), IPMask(net::ParseIP("ffff:ff80::")), net::ParseIP("2001:d80::")},
        {net::ParseIP("2001:db8::1"), IPMask(net::ParseIP("f0f0:0f0f::")), net::ParseIP("2000:d08::")},
    };
    for (auto& tt : ipMaskTests) {
        IP out = tt.in.Mask(tt.mask);
        EXPECT_TRUE(!out.empty() && tt.out.Equal(out))
            << "IP(" << tt.in << ").Mask(" << str(tt.mask.String()) << ") = " << out << ", want " << tt.out;
    }
}

TEST(GoNet, IPMaskString) {
    struct {
        IPMask in;
        std::string out;
    } ipMaskStringTests[] = {
        {net::IPv4Mask(255, 255, 255, 240), "fffffff0"},
        {net::IPv4Mask(255, 0, 128, 0), "ff008000"},
        {IPMask(net::ParseIP("ffff:ff80::")), "ffffff80000000000000000000000000"},
        {IPMask(net::ParseIP("ef00:ff80::cafe:0")), "ef00ff800000000000000000cafe0000"},
        {IPMask{}, "<nil>"},
    };
    for (auto& tt : ipMaskStringTests) EXPECT_EQ(str(tt.in.String()), tt.out);
}

TEST(GoNet, ParseCIDR) {
    struct {
        std::string in;
        IP ip;
        IP netip;
        IPMask mask;
        bool err;
    } parseCIDRTests[] = {
        {"135.104.0.0/32", net::IPv4(135, 104, 0, 0), net::IPv4(135, 104, 0, 0), net::IPv4Mask(255, 255, 255, 255), false},
        {"0.0.0.0/24", net::IPv4(0, 0, 0, 0), net::IPv4(0, 0, 0, 0), net::IPv4Mask(255, 255, 255, 0), false},
        {"135.104.0.0/24", net::IPv4(135, 104, 0, 0), net::IPv4(135, 104, 0, 0), net::IPv4Mask(255, 255, 255, 0), false},
        {"135.104.0.1/32", net::IPv4(135, 104, 0, 1), net::IPv4(135, 104, 0, 1), net::IPv4Mask(255, 255, 255, 255), false},
        {"135.104.0.1/24", net::IPv4(135, 104, 0, 1), net::IPv4(135, 104, 0, 0), net::IPv4Mask(255, 255, 255, 0), false},
        {"::1/128", net::ParseIP("::1"), net::ParseIP("::1"), IPMask(net::ParseIP("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")), false},
        {"abcd:2345::/127", net::ParseIP("abcd:2345::"), net::ParseIP("abcd:2345::"), IPMask(net::ParseIP("ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe")), false},
        {"abcd:2345::/65", net::ParseIP("abcd:2345::"), net::ParseIP("abcd:2345::"), IPMask(net::ParseIP("ffff:ffff:ffff:ffff:8000::")), false},
        {"abcd:2345::/64", net::ParseIP("abcd:2345::"), net::ParseIP("abcd:2345::"), IPMask(net::ParseIP("ffff:ffff:ffff:ffff::")), false},
        {"abcd:2345::/63", net::ParseIP("abcd:2345::"), net::ParseIP("abcd:2345::"), IPMask(net::ParseIP("ffff:ffff:ffff:fffe::")), false},
        {"abcd:2345::/33", net::ParseIP("abcd:2345::"), net::ParseIP("abcd:2345::"), IPMask(net::ParseIP("ffff:ffff:8000::")), false},
        {"abcd:2345::/32", net::ParseIP("abcd:2345::"), net::ParseIP("abcd:2345::"), IPMask(net::ParseIP("ffff:ffff::")), false},
        {"abcd:2344::/31", net::ParseIP("abcd:2344::"), net::ParseIP("abcd:2344::"), IPMask(net::ParseIP("ffff:fffe::")), false},
        {"abcd:2300::/24", net::ParseIP("abcd:2300::"), net::ParseIP("abcd:2300::"), IPMask(net::ParseIP("ffff:ff00::")), false},
        {"abcd:2345::/24", net::ParseIP("abcd:2345::"), net::ParseIP("abcd:2300::"), IPMask(net::ParseIP("ffff:ff00::")), false},
        {"2001:DB8::/48", net::ParseIP("2001:DB8::"), net::ParseIP("2001:DB8::"), IPMask(net::ParseIP("ffff:ffff:ffff::")), false},
        {"2001:DB8::1/48", net::ParseIP("2001:DB8::1"), net::ParseIP("2001:DB8::"), IPMask(net::ParseIP("ffff:ffff:ffff::")), false},
        {"192.168.1.1/255.255.255.0", {}, {}, {}, true},
        {"192.168.1.1/35", {}, {}, {}, true},
        {"2001:db8::1/-1", {}, {}, {}, true},
        {"2001:db8::1/-0", {}, {}, {}, true},
        {"-0.0.0.0/32", {}, {}, {}, true},
        {"0.-1.0.0/32", {}, {}, {}, true},
        {"0.0.-2.0/32", {}, {}, {}, true},
        {"0.0.0.-3/32", {}, {}, {}, true},
        {"0.0.0.0/-0", {}, {}, {}, true},
        {"127.000.000.001/32", {}, {}, {}, true},
        {"", {}, {}, {}, true},
    };
    for (auto& tt : parseCIDRTests) {
        auto [ip, n, err] = net::ParseCIDR(tt.in);
        if (tt.err) {
            auto* e = errors::As<net::ParseError>(err);
            EXPECT_TRUE(e && str(e->Type) == "CIDR address" && str(e->Text) == tt.in) << tt.in << ": " << err;
            continue;
        }
        ASSERT_FALSE(err) << tt.in << ": " << err;
        EXPECT_TRUE(tt.ip.Equal(ip) && tt.netip.Equal(n->IP) && deep_equal(n->Mask, tt.mask))
            << "ParseCIDR(" << tt.in << ") = " << ip << ", {" << n->IP << ", " << str(n->Mask.String()) << "}";
    }
}

TEST(GoNet, IPNetContains) {
    struct {
        IP ip;
        net::IPNet n;
        bool ok;
    } ipNetContainsTests[] = {
        {net::IPv4(172, 16, 1, 1), {net::IPv4(172, 16, 0, 0), net::CIDRMask(12, 32)}, true},
        {net::IPv4(172, 24, 0, 1), {net::IPv4(172, 16, 0, 0), net::CIDRMask(13, 32)}, false},
        {net::IPv4(192, 168, 0, 3), {net::IPv4(192, 168, 0, 0), net::IPv4Mask(0, 0, 255, 252)}, true},
        {net::IPv4(192, 168, 0, 4), {net::IPv4(192, 168, 0, 0), net::IPv4Mask(0, 255, 0, 252)}, false},
        {net::ParseIP("2001:db8:1:2::1"), {net::ParseIP("2001:db8:1::"), net::CIDRMask(47, 128)}, true},
        {net::ParseIP("2001:db8:1:2::1"), {net::ParseIP("2001:db8:2::"), net::CIDRMask(47, 128)}, false},
        {net::ParseIP("2001:db8:1:2::1"), {net::ParseIP("2001:db8:1::"), IPMask(net::ParseIP("ffff:0:ffff::"))}, true},
        {net::ParseIP("2001:db8:1:2::1"), {net::ParseIP("2001:db8:1::"), IPMask(net::ParseIP("0:0:0:ffff::"))}, false},
    };
    for (auto& tt : ipNetContainsTests) {
        EXPECT_EQ(tt.n.Contains(tt.ip), tt.ok) << "IPNet(" << str(tt.n.String()) << ").Contains(" << tt.ip << ")";
    }
}

TEST(GoNet, IPNetString) {
    struct {
        net::IPNet in;
        std::string out;
    } ipNetStringTests[] = {
        {{net::IPv4(192, 168, 1, 0), net::CIDRMask(26, 32)}, "192.168.1.0/26"},
        {{net::IPv4(192, 168, 1, 0), net::IPv4Mask(255, 0, 255, 0)}, "192.168.1.0/ff00ff00"},
        {{net::ParseIP("2001:db8::"), net::CIDRMask(55, 128)}, "2001:db8::/55"},
        {{net::ParseIP("2001:db8::"), IPMask(net::ParseIP("8000:f123:0:cafe::"))},
         "2001:db8::/8000f1230000cafe0000000000000000"},
    };
    for (auto& tt : ipNetStringTests) EXPECT_EQ(str(tt.in.String()), tt.out);
}

TEST(GoNet, CIDRMask) {
    struct {
        int ones, bits;
        IPMask out;
    } cidrMaskTests[] = {
        {0, 32, net::IPv4Mask(0, 0, 0, 0)},
        {12, 32, net::IPv4Mask(255, 240, 0, 0)},
        {24, 32, net::IPv4Mask(255, 255, 255, 0)},
        {32, 32, net::IPv4Mask(255, 255, 255, 255)},
        {0, 128, IPMask{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {4, 128, IPMask{0xf0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {48, 128, IPMask{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {128, 128, IPMask{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
        {33, 32, {}},
        {32, 33, {}},
        {-1, 128, {}},
        {128, -1, {}},
    };
    for (auto& tt : cidrMaskTests) {
        EXPECT_TRUE(deep_equal(net::CIDRMask(tt.ones, tt.bits), tt.out)) << "CIDRMask(" << tt.ones << ", " << tt.bits << ")";
    }
}

TEST(GoNet, SplitHostPort) {
    struct {
        std::string hostPort, host, port;
    } ok[] = {
        // Host name
        {"localhost:http", "localhost", "http"},
        {"localhost:80", "localhost", "80"},
        // Go-specific host name with zone identifier
        {"localhost%lo0:http", "localhost%lo0", "http"},
        {"localhost%lo0:80", "localhost%lo0", "80"},
        {"[localhost%lo0]:http", "localhost%lo0", "http"},  // Go 1 behavior
        {"[localhost%lo0]:80", "localhost%lo0", "80"},      // Go 1 behavior
        // IP literal
        {"127.0.0.1:http", "127.0.0.1", "http"},
        {"127.0.0.1:80", "127.0.0.1", "80"},
        {"[::1]:http", "::1", "http"},
        {"[::1]:80", "::1", "80"},
        // IP literal with zone identifier
        {"[::1%lo0]:http", "::1%lo0", "http"},
        {"[::1%lo0]:80", "::1%lo0", "80"},
        // Go-specific wildcard for host name
        {":http", "", "http"},  // Go 1 behavior
        {":80", "", "80"},      // Go 1 behavior
        // Go-specific wildcard for service name or transport port number
        {"golang.org:", "golang.org", ""},  // Go 1 behavior
        {"127.0.0.1:", "127.0.0.1", ""},    // Go 1 behavior
        {"[::1]:", "::1", ""},              // Go 1 behavior
        // Opaque service name
        {"golang.org:https%foo", "golang.org", "https%foo"},  // Go 1 behavior
    };
    for (auto& tt : ok) {
        auto [host, port, err] = net::SplitHostPort(tt.hostPort);
        EXPECT_TRUE(str(host) == tt.host && str(port) == tt.port && !err)
            << "SplitHostPort(" << tt.hostPort << ") = " << host << ", " << port << ", " << err;
    }

    struct {
        std::string hostPort, err;
    } bad[] = {
        {"golang.org", "missing port in address"},
        {"127.0.0.1", "missing port in address"},
        {"[::1]", "missing port in address"},
        {"[fe80::1%lo0]", "missing port in address"},
        {"[localhost%lo0]", "missing port in address"},
        {"localhost%lo0", "missing port in address"},

        {"::1", "too many colons in address"},
        {"fe80::1%lo0", "too many colons in address"},
        {"fe80::1%lo0:80", "too many colons in address"},

        // Test cases that didn't fail in Go 1
        {"[foo:bar]", "missing port in address"},
        {"[foo:bar]baz", "missing port in address"},
        {"[foo]bar:baz", "missing port in address"},

        {"[foo]:[bar]:baz", "too many colons in address"},

        {"[foo]:[bar]baz", "unexpected '[' in address"},
        {"foo[bar]:baz", "unexpected '[' in address"},

        {"foo]bar:baz", "unexpected ']' in address"},
    };
    for (auto& tt : bad) {
        auto [host, port, err] = net::SplitHostPort(tt.hostPort);
        if (!err) {
            ADD_FAILURE() << "SplitHostPort(" << tt.hostPort << ") should have failed";
            continue;
        }
        auto* e = errors::As<net::AddrError>(err);
        ASSERT_TRUE(e) << err;
        EXPECT_EQ(str(e->Err), tt.err) << tt.hostPort;
        EXPECT_TRUE(host.empty() && port.empty()) << tt.hostPort;
    }
}

TEST(GoNet, JoinHostPort) {
    struct {
        std::string host, port, hostPort;
    } tests[] = {
        // Host name
        {"localhost", "http", "localhost:http"},
        {"localhost", "80", "localhost:80"},
        // Go-specific host name with zone identifier
        {"localhost%lo0", "http", "localhost%lo0:http"},
        {"localhost%lo0", "80", "localhost%lo0:80"},
        // IP literal
        {"127.0.0.1", "http", "127.0.0.1:http"},
        {"127.0.0.1", "80", "127.0.0.1:80"},
        {"::1", "http", "[::1]:http"},
        {"::1", "80", "[::1]:80"},
        // IP literal with zone identifier
        {"::1%lo0", "http", "[::1%lo0]:http"},
        {"::1%lo0", "80", "[::1%lo0]:80"},
        // Go-specific wildcard for host name
        {"", "http", ":http"},  // Go 1 behavior
        {"", "80", ":80"},      // Go 1 behavior
        // Go-specific wildcard for service name or transport port number
        {"golang.org", "", "golang.org:"},  // Go 1 behavior
        {"127.0.0.1", "", "127.0.0.1:"},    // Go 1 behavior
        {"::1", "", "[::1]:"},              // Go 1 behavior
        // Opaque service name
        {"golang.org", "https%foo", "golang.org:https%foo"},  // Go 1 behavior
    };
    for (auto& tt : tests) EXPECT_EQ(str(net::JoinHostPort(tt.host, tt.port)), tt.hostPort);
}

TEST(GoNet, IPAddrFamily) {
    struct {
        IP in;
        bool af4, af6;
    } ipAddrFamilyTests[] = {
        {net::IPv4bcast, true, false},
        {net::IPv4allsys, true, false},
        {net::IPv4allrouter, true, false},
        {net::IPv4zero, true, false},
        {net::IPv4(224, 0, 0, 1), true, false},
        {net::IPv4(127, 0, 0, 1), true, false},
        {net::IPv4(240, 0, 0, 1), true, false},
        {net::IPv6unspecified, false, true},
        {net::IPv6loopback, false, true},
        {net::IPv6interfacelocalallnodes, false, true},
        {net::IPv6linklocalallnodes, false, true},
        {net::IPv6linklocalallrouters, false, true},
        {net::ParseIP("ff05::a:b:c:d"), false, true},
        {net::ParseIP("fe80::1:2:3:4"), false, true},
        {net::ParseIP("2001:db8::123:12:1"), false, true},
    };
    for (auto& tt : ipAddrFamilyTests) {
        EXPECT_EQ(!tt.in.To4().empty(), tt.af4) << tt.in;
        EXPECT_EQ(tt.in.size() == net::IPv6len && tt.in.To4().empty(), tt.af6) << tt.in;
    }
}

TEST(GoNet, IPAddrScope) {
    using scope_fn = bool (IP::*)() const;
    struct {
        scope_fn scope;
        const char* name;
        IP in;
        bool ok;
    } ipAddrScopeTests[] = {
#define S(f) &IP::f, #f
        {S(IsUnspecified), net::IPv4zero, true},
        {S(IsUnspecified), net::IPv4(127, 0, 0, 1), false},
        {S(IsUnspecified), net::IPv6unspecified, true},
        {S(IsUnspecified), net::IPv6interfacelocalallnodes, false},
        {S(IsUnspecified), {}, false},
        {S(IsLoopback), net::IPv4(127, 0, 0, 1), true},
        {S(IsLoopback), net::IPv4(127, 255, 255, 254), true},
        {S(IsLoopback), net::IPv4(128, 1, 2, 3), false},
        {S(IsLoopback), net::IPv6loopback, true},
        {S(IsLoopback), net::IPv6linklocalallrouters, false},
        {S(IsLoopback), {}, false},
        {S(IsMulticast), net::IPv4(224, 0, 0, 0), true},
        {S(IsMulticast), net::IPv4(239, 0, 0, 0), true},
        {S(IsMulticast), net::IPv4(240, 0, 0, 0), false},
        {S(IsMulticast), net::IPv6linklocalallnodes, true},
        {S(IsMulticast), IP{0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true},
        {S(IsMulticast), IP{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false},
        {S(IsMulticast), {}, false},
        {S(IsInterfaceLocalMulticast), net::IPv4(224, 0, 0, 0), false},
        {S(IsInterfaceLocalMulticast), net::IPv4(0xff, 0x01, 0, 0), false},
        {S(IsInterfaceLocalMulticast), net::IPv6interfacelocalallnodes, true},
        {S(IsInterfaceLocalMulticast), {}, false},
        {S(IsLinkLocalMulticast), net::IPv4(224, 0, 0, 0), true},
        {S(IsLinkLocalMulticast), net::IPv4(239, 0, 0, 0), false},
        {S(IsLinkLocalMulticast), net::IPv4(0xff, 0x02, 0, 0), false},
        {S(IsLinkLocalMulticast), net::IPv6linklocalallrouters, true},
        {S(IsLinkLocalMulticast), IP{0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false},
        {S(IsLinkLocalMulticast), {}, false},
        {S(IsLinkLocalUnicast), net::IPv4(169, 254, 0, 0), true},
        {S(IsLinkLocalUnicast), net::IPv4(169, 255, 0, 0), false},
        {S(IsLinkLocalUnicast), net::IPv4(0xfe, 0x80, 0, 0), false},
        {S(IsLinkLocalUnicast), IP{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true},
        {S(IsLinkLocalUnicast), IP{0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false},
        {S(IsLinkLocalUnicast), {}, false},
        {S(IsGlobalUnicast), net::IPv4(240, 0, 0, 0), true},
        {S(IsGlobalUnicast), net::IPv4(232, 0, 0, 0), false},
        {S(IsGlobalUnicast), net::IPv4(169, 254, 0, 0), false},
        {S(IsGlobalUnicast), net::IPv4bcast, false},
        {S(IsGlobalUnicast), IP{0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0x1, 0x23, 0, 0x12, 0, 0x1}, true},
        {S(IsGlobalUnicast), IP{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false},
        {S(IsGlobalUnicast), IP{0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false},
        {S(IsGlobalUnicast), {}, false},
        {S(IsPrivate), {}, false},
        {S(IsPrivate), net::IPv4(1, 1, 1, 1), false},
        {S(IsPrivate), net::IPv4(9, 255, 255, 255), false},
        {S(IsPrivate), net::IPv4(10, 0, 0, 0), true},
        {S(IsPrivate), net::IPv4(10, 255, 255, 255), true},
        {S(IsPrivate), net::IPv4(11, 0, 0, 0), false},
        {S(IsPrivate), net::IPv4(172, 15, 255, 255), false},
        {S(IsPrivate), net::IPv4(172, 16, 0, 0), true},
        {S(IsPrivate), net::IPv4(172, 16, 255, 255), true},
        {S(IsPrivate), net::IPv4(172, 23, 18, 255), true},
        {S(IsPrivate), net::IPv4(172, 31, 255, 255), true},
        {S(IsPrivate), net::IPv4(172, 31, 0, 0), true},
        {S(IsPrivate), net::IPv4(172, 32, 0, 0), false},
        {S(IsPrivate), net::IPv4(192, 167, 255, 255), false},
        {S(IsPrivate), net::IPv4(192, 168, 0, 0), true},
        {S(IsPrivate), net::IPv4(192, 168, 255, 255), true},
        {S(IsPrivate), net::IPv4(192, 169, 0, 0), false},
        {S(IsPrivate), IP{0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, false},
        {S(IsPrivate), IP{0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, true},
        {S(IsPrivate), IP{0xfc, 0xff, 0x12, 0, 0, 0, 0, 0x44, 0, 0, 0, 0, 0, 0, 0, 0}, true},
        {S(IsPrivate), IP{0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, true},
        {S(IsPrivate), IP{0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false},
#undef S
    };
    for (auto& tt : ipAddrScopeTests) {
        EXPECT_EQ((tt.in.*tt.scope)(), tt.ok) << tt.name << "(" << tt.in << ")";
        IP ip = tt.in.To4();
        if (ip.empty()) continue;
        EXPECT_EQ((ip.*tt.scope)(), tt.ok) << tt.name << "(" << ip << ")";
    }
}

// ---------------------------------------------------------------------------
// TestResolveTCPAddr and TestResolveUDPAddr

namespace {

template <typename A>
struct resolve_test {
    std::string network, litAddrOrName;
    ptr<A> addr;
    want_err err;
};

template <typename A>
ptr<A> mk(IP ip, int port, std::string zone = {}) {
    return make_ptr<A>(std::move(ip), port, string(zone));
}

template <typename A>
std::vector<resolve_test<A>> resolve_tests(const std::string& p, const std::string& svc, int svcport) {
    auto n = [&](const char* suffix) { return p + suffix; };
    return {
        {n(""), "127.0.0.1:0", mk<A>(net::IPv4(127, 0, 0, 1), 0), {}},
        {n("4"), "127.0.0.1:65535", mk<A>(net::IPv4(127, 0, 0, 1), 65535), {}},

        {n(""), "[::1]:0", mk<A>(net::ParseIP("::1"), 0), {}},
        {n("6"), "[::1]:65535", mk<A>(net::ParseIP("::1"), 65535), {}},

        {n(""), "[::1%en0]:1", mk<A>(net::ParseIP("::1"), 1, "en0"), {}},
        {n("6"), "[::1%911]:2", mk<A>(net::ParseIP("::1"), 2, "911"), {}},

        {"", "127.0.0.1:0", mk<A>(net::IPv4(127, 0, 0, 1), 0), {}},  // Go 1.0 behavior
        {"", "[::1]:0", mk<A>(net::ParseIP("::1"), 0), {}},          // Go 1.0 behavior

        {n(""), ":12345", mk<A>({}, 12345), {}},

        {"http", "127.0.0.1:0", nullptr, UnknownNetworkError("http")},

        {n(""), "127.0.0.1:" + svc, mk<A>(net::ParseIP("127.0.0.1"), svcport), {}},
        {n(""), "[::ffff:127.0.0.1]:" + svc, mk<A>(net::ParseIP("::ffff:127.0.0.1"), svcport), {}},
        {n(""), "[2001:db8::1]:" + svc, mk<A>(net::ParseIP("2001:db8::1"), svcport), {}},
        {n("4"), "127.0.0.1:" + svc, mk<A>(net::ParseIP("127.0.0.1"), svcport), {}},
        {n("4"), "[::ffff:127.0.0.1]:" + svc, mk<A>(net::ParseIP("127.0.0.1"), svcport), {}},
        {n("6"), "[2001:db8::1]:" + svc, mk<A>(net::ParseIP("2001:db8::1"), svcport), {}},

        {n("4"), "[2001:db8::1]:" + svc, nullptr, AddrError(errNoSuitableAddress, "2001:db8::1")},
        {n("6"), "127.0.0.1:" + svc, nullptr, AddrError(errNoSuitableAddress, "127.0.0.1")},
        {n("6"), "[::ffff:127.0.0.1]:" + svc, nullptr, AddrError(errNoSuitableAddress, "::ffff:127.0.0.1")},
    };
}

template <typename A, typename Resolve>
void test_resolve(const std::vector<resolve_test<A>>& tests, Resolve resolve) {
    for (auto& tt : tests) {
        auto [addr, err] = resolve(tt.network, tt.litAddrOrName);
        if (!addr_equal(addr, tt.addr) || !err_equal(err, tt.err)) {
            ADD_FAILURE() << "Resolve(" << tt.network << ", " << tt.litAddrOrName << ") = " << addr << ", " << err
                          << ", want " << tt.addr;
            continue;
        }
        if (!err) {
            auto [addr2, err2] = resolve(str(addr->Network()), str(addr->String()));
            EXPECT_TRUE(addr_equal(addr2, tt.addr) && !err2)
                << "(" << tt.network << ", " << tt.litAddrOrName << "): Resolve(" << addr->Network() << ", "
                << addr->String() << ") = " << addr2 << ", " << err2;
        }
    }
}

}  // namespace

TEST(GoNet, ResolveTCPAddr) {
    test_resolve(resolve_tests<net::TCPAddr>("tcp", "http", 80),
                 [](const std::string& n, const std::string& a) { return net::ResolveTCPAddr(n, a); });
}

// ---------------------------------------------------------------------------
// udpsock_test.go

TEST(GoNet, ResolveUDPAddr) {
    test_resolve(resolve_tests<net::UDPAddr>("udp", "domain", 53),
                 [](const std::string& n, const std::string& a) { return net::ResolveUDPAddr(n, a); });
}

namespace {

void assertErrWriteToConnected(const error& err) {
    auto* e = errors::As<net::OpError>(err);
    EXPECT_TRUE(e && e->Err == net::ErrWriteToConnected) << "got " << err << "; want ErrWriteToConnected";
}

void testWriteToConn(const std::string& raddr) {
    auto [c, err] = net::Dial("udp", raddr);
    ASSERT_FALSE(err) << err;
    auto [ra, rerr] = net::ResolveUDPAddr("udp", raddr);
    ASSERT_FALSE(rerr) << rerr;
    auto* uc = dynamic_cast<net::UDPConn*>(c.get());
    ASSERT_TRUE(uc);

    auto bb = b("CONNECTED-MODE SOCKET");
    assertErrWriteToConnected(uc->WriteToUDP(bb, ra).second);
    assertErrWriteToConnected(uc->WriteTo(bb, ra).second);
    EXPECT_FALSE(c->Write(bb).second);
    assertErrWriteToConnected(std::get<2>(uc->WriteMsgUDP(bb, {}, ra)));
    EXPECT_FALSE(std::get<2>(uc->WriteMsgUDP(bb, {}, nullptr)));
    c->Close();
}

void testWriteToPacketConn(const std::string& raddr) {
    auto [c, err] = net::ListenPacket("udp", "127.0.0.1:0");
    ASSERT_FALSE(err) << err;
    auto [ra, rerr] = net::ResolveUDPAddr("udp", raddr);
    ASSERT_FALSE(rerr) << rerr;
    auto* uc = dynamic_cast<net::UDPConn*>(c.get());
    ASSERT_TRUE(uc);

    auto bb = b("UNCONNECTED-MODE SOCKET");
    ASSERT_FALSE(uc->WriteToUDP(bb, ra).second);
    ASSERT_FALSE(c->WriteTo(bb, ra).second);
    ASSERT_TRUE(uc->Write(bb).second) << "should fail";
    error werr = std::get<2>(uc->WriteMsgUDP(bb, {}, nullptr));
    ASSERT_TRUE(werr) << "should fail";
    auto* e = errors::As<net::OpError>(werr);
    ASSERT_TRUE(e && e->Err.Error() == "missing address") << "should fail as errMissingAddress: " << werr;
    ASSERT_FALSE(std::get<2>(uc->WriteMsgUDP(bb, {}, ra)));
    c->Close();
}

}  // namespace

TEST(GoNet, WriteToUDP) {
    auto [c, err] = net::ListenPacket("udp", "127.0.0.1:0");
    ASSERT_FALSE(err) << err;
    testWriteToConn(str(c->LocalAddr()->String()));
    testWriteToPacketConn(str(c->LocalAddr()->String()));
    c->Close();
}

TEST(GoNet, UDPConnLocalName) {
    ptr<net::UDPAddr> laddrs[] = {make_ptr<net::UDPAddr>(net::IPv4(127, 0, 0, 1), 0), make_ptr<net::UDPAddr>(), nullptr};
    for (auto& laddr : laddrs) {
        auto [c, err] = net::ListenUDP("udp4", laddr);
        ASSERT_FALSE(err) << err;
        auto la = c->LocalAddr();
        auto* a = dynamic_cast<net::UDPAddr*>(la.get());
        EXPECT_TRUE(a && a->Port != 0) << "got " << la << "; expected a proper address with non-zero port number";
        c->Close();
    }
}

TEST(GoNet, UDPConnLocalAndRemoteNames) {
    for (std::string laddr : {"", "127.0.0.1:0"}) {
        auto [c1, err] = net::ListenPacket("udp", "127.0.0.1:0");
        ASSERT_FALSE(err) << err;
        ptr<net::UDPAddr> la;
        if (!laddr.empty()) {
            error rerr;
            std::tie(la, rerr) = net::ResolveUDPAddr("udp", laddr);
            ASSERT_FALSE(rerr) << rerr;
        }
        auto c1la = ptr<net::UDPAddr>(dynamic_cast<net::UDPAddr*>(c1->LocalAddr().get()));
        auto [c2, derr] = net::DialUDP("udp", la, c1la);
        ASSERT_FALSE(derr) << derr;

        struct {
            ptr<net::Addr> got;
            bool ok;
        } connAddrs[] = {
            {c1->LocalAddr(), true},
            {dynamic_cast<net::UDPConn*>(c1.get())->RemoteAddr(), false},
            {c2->LocalAddr(), true},
            {c2->RemoteAddr(), true},
        };
        for (auto& ca : connAddrs) {
            auto* a = dynamic_cast<net::UDPAddr*>(ca.got.get());
            bool ok = a != nullptr;
            EXPECT_FALSE(ok != ca.ok || (ok && a->Port == 0))
                << "got " << ca.got << "; expected a proper address with non-zero port number";
        }
        c2->Close();
        c1->Close();
    }
}

TEST(GoNet, UDPZeroBytePayload) {
    auto c = newLocalPacketListener();
    for (bool genericRead : {false, true}) {
        auto [n, err] = c->WriteTo({}, c->LocalAddr());
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(n, 0);
        c->SetReadDeadline(time::Now().Add(30 * time::Second));
        byte bb[1];
        error rerr;
        if (genericRead) rerr = dynamic_cast<net::Conn*>(c.get())->Read(bb).second;
        else rerr = std::get<2>(c->ReadFrom(bb));
        EXPECT_FALSE(rerr) << (genericRead ? "Read" : "ReadFrom") << " of zero byte packet failed: " << rerr;
    }
    c->Close();
}

TEST(GoNet, UDPZeroByteBuffer) {
    auto c = newLocalPacketListener();
    auto bb = b("UDP ZERO BYTE BUFFER TEST");
    for (bool genericRead : {false, true}) {
        auto [n, err] = c->WriteTo(bb, c->LocalAddr());
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(n, static_cast<isize>(bb.size()));
        c->SetReadDeadline(time::Now().Add(100 * time::Millisecond));
        error rerr;
        if (genericRead) rerr = dynamic_cast<net::Conn*>(c.get())->Read({}).second;
        else rerr = std::get<2>(c->ReadFrom({}));
        // ReadFrom succeeds; Read may time out, depending on the platform.
        if (rerr) {
            EXPECT_TRUE(rerr->Timeout()) << rerr;
        }
    }
    c->Close();
}

TEST(GoNet, UDPReadSizeError) {
    auto c1 = newLocalPacketListener();
    auto [c2, err] = net::Dial("udp", str(c1->LocalAddr()->String()));
    ASSERT_FALSE(err) << err;
    auto b1 = b("READ SIZE ERROR TEST");
    for (bool genericRead : {false, true}) {
        auto [n, werr] = c2->Write(b1);
        ASSERT_FALSE(werr) << werr;
        EXPECT_EQ(n, static_cast<isize>(b1.size()));
        std::vector<byte> b2(b1.size() - 1);
        isize m;
        error rerr;
        if (genericRead) {
            std::tie(m, rerr) = dynamic_cast<net::Conn*>(c1.get())->Read(b2);
        } else {
            ptr<net::Addr> from;
            std::tie(m, from, rerr) = c1->ReadFrom(b2);
        }
        ASSERT_FALSE(rerr) << rerr;
        ASSERT_EQ(m, static_cast<isize>(b1.size() - 1));
    }
    c2->Close();
    c1->Close();
}

// TestUDPReadTimeout verifies that ReadFromUDP with timeout returns an error
// without data or an address.
TEST(GoNet, UDPReadTimeout) {
    auto [la, err] = net::ResolveUDPAddr("udp4", "127.0.0.1:0");
    ASSERT_FALSE(err) << err;
    auto [c, lerr] = net::ListenUDP("udp4", la);
    ASSERT_FALSE(lerr) << lerr;
    c->SetDeadline(time::Now());
    byte bb[1];
    auto [n, addr, rerr] = c->ReadFromUDP(bb);
    EXPECT_TRUE(errors::Is(rerr, os::ErrDeadlineExceeded)) << "ReadFromUDP got err " << rerr;
    EXPECT_EQ(n, 0);
    EXPECT_FALSE(addr) << "ReadFromUDP got addr " << addr << " want nil";
    c->Close();
}

// The ReadMsgUDP half of TestUDPIPVersionReadMsg (the other is netip's).
TEST(GoNet, UDPIPVersionReadMsg) {
    auto [conn, err] = net::ListenUDP("udp4", make_ptr<net::UDPAddr>(net::IPv4(127, 0, 0, 1), 0));
    ASSERT_FALSE(err) << err;
    auto daddr = ptr<net::UDPAddr>(dynamic_cast<net::UDPAddr*>(conn->LocalAddr().get()));
    std::vector<byte> buf(8);
    ASSERT_FALSE(conn->WriteToUDP(buf, daddr).second);
    auto [n, oobn, flags, soldaddr, rerr] = conn->ReadMsgUDP(buf, {});
    ASSERT_FALSE(rerr) << rerr;
    ASSERT_TRUE(soldaddr);
    EXPECT_EQ(soldaddr->IP.size(), 4u) << "returned UDPAddr is not IPv4";
    conn->Close();
}

// ---------------------------------------------------------------------------
// packetconn_test.go ("unixgram" is in go_net_unix_test.cpp)

TEST(GoNet, PacketConn) {
    auto wb = b("PACKETCONN TEST");
    auto [c1, err] = net::ListenPacket("udp", "127.0.0.1:0");
    ASSERT_FALSE(err) << err;
    auto [c2, err2] = net::ListenPacket("udp", "127.0.0.1:0");
    ASSERT_FALSE(err2) << err2;
    std::vector<byte> rb2(128), rb1(128);
    ASSERT_FALSE(c1->WriteTo(wb, c2->LocalAddr()).second);
    ASSERT_FALSE(std::get<2>(c2->ReadFrom(rb2)));
    ASSERT_FALSE(c2->WriteTo(wb, c1->LocalAddr()).second);
    ASSERT_FALSE(std::get<2>(c1->ReadFrom(rb1)));
    c1->Close();
    c2->Close();
}

TEST(GoNet, ConnAndPacketConn) {
    auto wb = b("PACKETCONN TEST");
    auto [c1, err] = net::ListenPacket("udp", "127.0.0.1:0");
    ASSERT_FALSE(err) << err;
    auto [c2, derr] = net::Dial("udp", str(c1->LocalAddr()->String()));
    ASSERT_FALSE(derr) << derr;
    c2->LocalAddr();
    c2->RemoteAddr();
    ASSERT_FALSE(c2->Write(wb).second);
    std::vector<byte> rb1(128), rb2(128);
    ASSERT_FALSE(std::get<2>(c1->ReadFrom(rb1)));
    ASSERT_FALSE(c1->WriteTo(wb, c2->LocalAddr()).second);
    ASSERT_FALSE(c2->Read(rb2).second);
    c2->Close();
    c1->Close();
}
