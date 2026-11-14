// Ported from Go 1.27.3's net/netip/netip_test.go, netip_pkg_test.go,
// uint128_test.go and slow_test.go (parseIPSlow), with Go's test names and
// tables.
//
// Left out: the JSON round trips (go++'s encoding/json doesn't use
// TextMarshaler; MarshalText and UnmarshalText are tested directly), the
// panicking cases of TestAs4 (a go++ panic can't be recovered), the
// allocation tests, fuzz tests and benchmarks.
#include <gopp/net.hpp>
#include <gopp/net/netip.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::netip;

namespace gopp::netip {
std::ostream& operator<<(std::ostream& os, const Addr& a) { return os << a.String().view(); }
std::ostream& operator<<(std::ostream& os, const AddrPort& a) { return os << a.String().view(); }
std::ostream& operator<<(std::ostream& os, const Prefix& p) { return os << p.String().view(); }
std::ostream& operator<<(std::ostream& os, const uint128& u) { return os << "{" << u.hi << " " << u.lo << "}"; }
}  // namespace gopp::netip

namespace {

Addr mustIP(std::string_view s) { return MustParseAddr(s); }
AddrPort mustIPPort(std::string_view s) { return MustParseAddrPort(s); }
Prefix mustPrefix(std::string_view s) { return MustParsePrefix(s); }
Addr MkAddr(uint128 u, const netip::detail::addrDetail* z) { return Addr(u, z); }
uint128 Mk128(uint64 hi, uint64 lo) { return {hi, lo}; }
const netip::detail::addrDetail* const Z4 = z4;
const netip::detail::addrDetail* const Z6noz = z6noz;

std::vector<Addr> mustIPs(std::initializer_list<std::string_view> strs) {
    std::vector<Addr> res;
    for (auto s : strs) res.push_back(mustIP(s));
    return res;
}

std::string str(const string& s) { return std::string(s.view()); }
std::string str(const slice<byte>& b) { return std::string(asStringView(b)); }

template <typename T>
std::string join(const std::vector<T>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); i++) {
        if (i) s += " ";
        s += str(v[i].String());
    }
    return s + "]";
}

// testAppendToMarshal: AppendTo and MarshalText agree.
template <typename T>
void TestAppendToMarshal(const T& x) {
    auto [m, err] = x.MarshalText();
    ASSERT_FALSE(err) << err;
    auto a = x.AppendTo(slice<byte>::make(0, m.size()));
    EXPECT_EQ(str(m), str(a)) << x;
}

// --- slow_test.go -----------------------------------------------------

std::vector<std::string> split(std::string_view s, std::string_view sep) {
    std::vector<std::string> out;
    for (;;) {
        auto i = s.find(sep);
        if (i == s.npos) {
            out.emplace_back(s);
            return out;
        }
        out.emplace_back(s.substr(0, i));
        s.remove_prefix(i + sep.size());
    }
}

std::size_t count(std::string_view s, char c) { return std::size_t(std::count(s.begin(), s.end(), c)); }

bool parseUintStrict(std::string_view s, int base, uint64 max, uint64* v) {
    if (s.empty()) return false;
    uint64 n = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        n = n * uint64(base) + uint64(d);
        if (n > max) return false;
    }
    *v = n;
    return true;
}

std::pair<Addr, bool> parseIPv4Slow(std::string_view s) {
    auto fs = split(s, ".");
    if (fs.size() != 4) return {Addr{}, false};
    std::array<byte, 4> ret{};
    for (int i = 0; i < 4; i++) {
        uint64 v;
        if (!parseUintStrict(fs[i], 10, 255, &v)) return {Addr{}, false};
        ret[i] = byte(v);
    }
    return {AddrFrom4(ret), true};
}

std::pair<std::string, bool> normalizeIPv6Slow(std::string_view orig) {
    std::string s(orig);
    auto i = s.rfind(':');
    if (i == s.npos) return {{}, false};
    if (s.substr(i + 1).find('.') != s.npos) {
        auto [ip, ok] = parseIPv4Slow(std::string_view(s).substr(i + 1));
        if (!ok) return {{}, false};
        auto a4 = ip.As4();
        char buf[16];
        std::snprintf(buf, sizeof buf, ":%02x%02x:%02x%02x", a4[0], a4[1], a4[2], a4[3]);
        s = s.substr(0, i) + buf;
    }
    auto fs = split(s, "::");
    if (fs.size() == 2) {
        auto lhs = fs[0], rhs = fs[1];
        std::size_t nblocks = count(lhs, ':') + count(rhs, ':');
        if (!lhs.empty()) nblocks++;
        if (!rhs.empty()) nblocks++;
        if (nblocks > 7) return {{}, false};
        std::vector<std::string> parts;
        if (!lhs.empty()) parts.push_back(lhs);
        for (std::size_t k = 0; k < 8 - nblocks; k++) parts.push_back("0");
        if (!rhs.empty()) parts.push_back(rhs);
        s.clear();
        for (std::size_t k = 0; k < parts.size(); k++) s += (k ? ":" : "") + parts[k];
    } else if (fs.size() != 1) {
        return {{}, false};
    }
    return {s, true};
}

std::pair<Addr, bool> parseIPSlow(std::string_view s0) {
    std::string s(s0), zone;
    auto fs = split(s, "%");
    if (fs.size() == 2) {
        s = fs[0];
        zone = fs[1];
        if (zone.empty()) return {Addr{}, false};
    } else if (fs.size() != 1) {
        return {Addr{}, false};
    }
    if (count(s, ':') == 0) {
        if (!zone.empty()) return {Addr{}, false};
        return parseIPv4Slow(s);
    }
    auto [normal, ok] = normalizeIPv6Slow(s);
    if (!ok) return {Addr{}, false};
    fs = split(normal, ":");
    if (fs.size() != 8) return {Addr{}, false};
    std::array<byte, 16> ret{};
    for (int i = 0; i < 8; i++) {
        uint64 v;
        if (fs[i].size() > 4 || !parseUintStrict(fs[i], 16, 0xffff, &v)) return {Addr{}, false};
        ret[i * 2] = byte(v >> 8);
        ret[i * 2 + 1] = byte(v);
    }
    return {AddrFrom16(ret).WithZone(zone), true};
}

}  // namespace

// --- netip_test.go ------------------------------------------------------

TEST(GoNetip, ParseAddr) {
    struct test {
        std::string in;
        Addr ip = {};
        std::string str_ = {};
        std::string wantErr = {};
    };
    std::vector<test> validIPs = {
        {"0.0.0.0", MkAddr(Mk128(0, 0xffff00000000), Z4)},
        {"192.168.140.255", MkAddr(Mk128(0, 0xffffc0a88cff), Z4)},
        {"010.000.015.001", {}, "", R"(ParseAddr("010.000.015.001"): IPv4 field has octet with leading zero)"},
        {"000001.00000002.00000003.000000004", {}, "",
         R"(ParseAddr("000001.00000002.00000003.000000004"): IPv4 field has octet with leading zero)"},
        {"::ffff:1.2.03.4", {}, "", R"(ParseAddr("::ffff:1.2.03.4"): IPv4 field has octet with leading zero)"},
        {"::ffff:1.2.3.z", {}, "", R"(ParseAddr("::ffff:1.2.3.z"): unexpected character (at "z"))"},
        {"::", MkAddr(Mk128(0, 0), Z6noz)},
        {"::1", MkAddr(Mk128(0, 1), Z6noz)},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b:430b", MkAddr(Mk128(0xfd7a115ca1e0ab12, 0x4843cd96626b430b), Z6noz)},
        {"fd7a:115c::626b:430b", MkAddr(Mk128(0xfd7a115c00000000, 0x00000000626b430b), Z6noz)},
        {"fd7a:115c:a1e0:ab12:4843:cd96::", MkAddr(Mk128(0xfd7a115ca1e0ab12, 0x4843cd9600000000), Z6noz)},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b::", MkAddr(Mk128(0xfd7a115ca1e0ab12, 0x4843cd96626b0000), Z6noz),
         "fd7a:115c:a1e0:ab12:4843:cd96:626b:0"},
        {"fd7a:115c:a1e0::4843:cd96:626b:430b", MkAddr(Mk128(0xfd7a115ca1e00000, 0x4843cd96626b430b), Z6noz),
         "fd7a:115c:a1e0:0:4843:cd96:626b:430b"},
        {"::ffff:192.168.140.255", MkAddr(Mk128(0, 0x0000ffffc0a88cff), Z6noz), "::ffff:192.168.140.255"},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b:430b%eth0",
         MkAddr(Mk128(0xfd7a115ca1e0ab12, 0x4843cd96626b430b), MakeAddrDetail(true, "eth0"))},
        {"1:2::ffff:192.168.140.255%eth1",
         MkAddr(Mk128(0x0001000200000000, 0x0000ffffc0a88cff), MakeAddrDetail(true, "eth1")),
         "1:2::ffff:c0a8:8cff%eth1"},
        {"::ffff:192.168.140.255%eth1", MkAddr(Mk128(0, 0x0000ffffc0a88cff), MakeAddrDetail(true, "eth1")),
         "::ffff:192.168.140.255%eth1"},
        {"FD9E:1A04:F01D::1", MkAddr(Mk128(0xfd9e1a04f01d0000, 0x1), Z6noz), "fd9e:1a04:f01d::1"},
    };
    for (auto& test : validIPs) {
        SCOPED_TRACE(test.in);
        auto [got, err] = ParseAddr(test.in);
        if (err) {
            if (str(err.Error()) == test.wantErr) continue;
            FAIL() << err;
        }
        ASSERT_TRUE(test.wantErr.empty()) << "wanted error " << test.wantErr << "; got none";
        EXPECT_EQ(got, test.ip);
        // ParseAddr is a pure function.
        auto [got2, err2] = ParseAddr(test.in);
        ASSERT_FALSE(err2);
        EXPECT_EQ(got, got2);
        // ParseAddr(ip.String()) is the identity.
        auto [got3, err3] = ParseAddr(got.String().view());
        ASSERT_FALSE(err3);
        EXPECT_EQ(got, got3);
        // The slow-but-readable parser agrees.
        auto [slow, ok] = parseIPSlow(test.in);
        ASSERT_TRUE(ok);
        EXPECT_EQ(got, slow);
        std::string wants = test.str_.empty() ? test.in : test.str_;
        EXPECT_EQ(str(got.String()), wants);
        TestAppendToMarshal(got);
        // MarshalText and UnmarshalText work like String and ParseAddr.
        Addr tgot;
        ASSERT_FALSE(tgot.UnmarshalText(as_bytes(test.in)));
        EXPECT_EQ(tgot, got);
        EXPECT_EQ(str(tgot.MarshalText().first), wants);
    }

    std::vector<std::string> invalidIPs = {
        "",
        "bad",
        "1234",
        "1.2.3.4%eth0",
        ".1.2.3",
        "1.2.3.",
        "1..2.3",
        "1.2.3.4.5",
        "0300.0250.0214.0377",
        "0xc0.0xa8.0x8c.0xff",
        "192.168.12345",
        "127.0.1",
        "192.1234567",
        "127.1",
        "192.168.300.1",
        "192.168.0.1.5.6",
        "1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:9",
        "1:2:3:4::5:6:7:8",
        "fe801::1",
        "fe80:tail:scal:e::",
        "fe80::1%",
        "ffff:ffff:ffff:ffff:ffff:ffff:ffff:192.168.140.255",
        "ffff::ffff:ffff:ffff:ffff:ffff:ffff:192.168.140.255",
        "::ffff:192.168.140.bad",
        "fe80::1::1",
        "fe80:1?:1",
        "fe80:",
        "0:0:0:0:0:ffff:0:00000",
        "0:0:0:0:00000:ffff:127.1.2.3",
    };
    for (auto& s : invalidIPs) {
        SCOPED_TRACE(s);
        auto [got, err] = ParseAddr(s);
        EXPECT_TRUE(err) << "ParseAddr = " << got << ", want error";
        EXPECT_FALSE(parseIPSlow(s).second);
        EXPECT_TRUE(net::ParseIP(s).empty());
        if (s.empty()) continue;
        Addr tgot;
        EXPECT_TRUE(tgot.UnmarshalText(as_bytes(s)));
    }
}

TEST(GoNetip, AddrFromSlice) {
    struct {
        std::vector<byte> ip;
        Addr wantAddr;
        bool wantOK;
    } tests[] = {
        {{10, 0, 0, 1}, mustIP("10.0.0.1"), true},
        {{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}, mustIP("fe80::01"), true},
        {{0, 1, 2}, Addr{}, false},
        {{}, Addr{}, false},
    };
    for (auto& tt : tests) {
        auto [addr, ok] = AddrFromSlice(tt.ip);
        EXPECT_EQ(ok, tt.wantOK);
        EXPECT_EQ(addr, tt.wantAddr);
    }
}

TEST(GoNetip, IPv4Constructors) { EXPECT_EQ(AddrFrom4({1, 2, 3, 4}), MustParseAddr("1.2.3.4")); }

TEST(GoNetip, AddrAppendText) {
    struct {
        Addr ip;
        std::string want;
    } tests[] = {
        {Addr{}, ""},
        {mustIP("1.2.3.4"), "1.2.3.4"},
        {mustIP("fd7a:115c:a1e0:ab12:4843:cd96:626b:430b"), "fd7a:115c:a1e0:ab12:4843:cd96:626b:430b"},
        {mustIP("::ffff:192.168.140.255"), "::ffff:192.168.140.255"},
        {mustIP("::ffff:192.168.140.255%en0"), "::ffff:192.168.140.255%en0"},
    };
    for (auto& tc : tests) {
        auto [mtAppend, err] = tc.ip.AppendText(slice<byte>::make(4, 32));
        ASSERT_FALSE(err);
        EXPECT_EQ(str(mtAppend.Sub(4)), tc.want);
    }
}

TEST(GoNetip, AddrMarshalUnmarshalBinary) {
    struct {
        std::string ip;
        std::size_t wantSize;
    } tests[] = {
        {"", 0},
        {"1.2.3.4", 4},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b:430b", 16},
        {"::ffff:c000:0280", 16},
        {"::ffff:c000:0280%eth0", 20},
    };
    for (auto& tc : tests) {
        Addr ip;
        if (!tc.ip.empty()) ip = mustIP(tc.ip);
        auto [b, err] = ip.MarshalBinary();
        ASSERT_FALSE(err);
        ASSERT_EQ(b.size(), tc.wantSize) << tc.ip;
        Addr ip2;
        ASSERT_FALSE(ip2.UnmarshalBinary(b));
        EXPECT_EQ(ip, ip2);

        auto [bAppend, err2] = ip.AppendBinary(slice<byte>::make(4, 32));
        ASSERT_FALSE(err2);
        bAppend = bAppend.Sub(4);
        ASSERT_EQ(bAppend.size(), tc.wantSize) << tc.ip;
        Addr ip3;
        ASSERT_FALSE(ip3.UnmarshalBinary(bAppend));
        EXPECT_EQ(ip, ip3);
    }
    // Not from an unexpected IP length.
    for (std::size_t n : {3, 5}) {
        Addr ip2;
        std::vector<byte> b(n, 1);
        EXPECT_TRUE(ip2.UnmarshalBinary(b)) << n;
    }
}

TEST(GoNetip, AddrPortMarshalTextString) {
    struct {
        AddrPort in;
        std::string want;
    } tests[] = {
        {mustIPPort("1.2.3.4:80"), "1.2.3.4:80"},
        {mustIPPort("[::]:80"), "[::]:80"},
        {mustIPPort("[1::CAFE]:80"), "[1::cafe]:80"},
        {mustIPPort("[1::CAFE%en0]:80"), "[1::cafe%en0]:80"},
        {mustIPPort("[::FFFF:192.168.140.255]:80"), "[::ffff:192.168.140.255]:80"},
        {mustIPPort("[::FFFF:192.168.140.255%en0]:80"), "[::ffff:192.168.140.255%en0]:80"},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(str(tt.in.String()), tt.want);
        auto [mt, err] = tt.in.MarshalText();
        ASSERT_FALSE(err);
        EXPECT_EQ(str(mt), tt.want);
        auto [mtAppend, err2] = tt.in.AppendText(slice<byte>::make(4, 32));
        ASSERT_FALSE(err2);
        EXPECT_EQ(str(mtAppend.Sub(4)), tt.want);
    }
}

TEST(GoNetip, AddrPortMarshalUnmarshalBinary) {
    struct {
        std::string ipport;
        std::size_t wantSize;
    } tests[] = {
        {"1.2.3.4:51820", 4 + 2},
        {"[fd7a:115c:a1e0:ab12:4843:cd96:626b:430b]:80", 16 + 2},
        {"[::ffff:c000:0280]:65535", 16 + 2},
        {"[::ffff:c000:0280%eth0]:1", 20 + 2},
    };
    for (auto& tc : tests) {
        AddrPort ipport = mustIPPort(tc.ipport);
        auto [b, err] = ipport.MarshalBinary();
        ASSERT_FALSE(err);
        ASSERT_EQ(b.size(), tc.wantSize);
        AddrPort ipport2;
        ASSERT_FALSE(ipport2.UnmarshalBinary(b));
        EXPECT_EQ(ipport, ipport2);
        auto [bAppend, err2] = ipport.AppendBinary(slice<byte>::make(4, 32));
        ASSERT_FALSE(err2);
        bAppend = bAppend.Sub(4);
        ASSERT_EQ(bAppend.size(), tc.wantSize);
        AddrPort ipport3;
        ASSERT_FALSE(ipport3.UnmarshalBinary(bAppend));
        EXPECT_EQ(ipport, ipport3);
    }
    for (std::size_t n : {3, 7}) {
        AddrPort ipport2;
        std::vector<byte> b(n, 1);
        EXPECT_TRUE(ipport2.UnmarshalBinary(b)) << n;
    }
}

TEST(GoNetip, PrefixMarshalTextString) {
    struct {
        Prefix in;
        std::string want;
    } tests[] = {
        {mustPrefix("1.2.3.4/24"), "1.2.3.4/24"},
        {mustPrefix("fd7a:115c:a1e0:ab12:4843:cd96:626b:430b/118"), "fd7a:115c:a1e0:ab12:4843:cd96:626b:430b/118"},
        {mustPrefix("::ffff:c000:0280/96"), "::ffff:192.0.2.128/96"},
        {mustPrefix("::ffff:192.168.140.255/8"), "::ffff:192.168.140.255/8"},
        {PrefixFrom(mustIP("::ffff:c000:0280").WithZone("eth0"), 37), "::ffff:192.0.2.128/37"},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(str(tt.in.String()), tt.want);
        auto [mt, err] = tt.in.MarshalText();
        ASSERT_FALSE(err);
        EXPECT_EQ(str(mt), tt.want);
        auto [mtAppend, err2] = tt.in.AppendText(slice<byte>::make(4, 64));
        ASSERT_FALSE(err2);
        EXPECT_EQ(str(mtAppend.Sub(4)), tt.want);
    }
}

TEST(GoNetip, PrefixMarshalUnmarshalBinary) {
    struct testCase {
        Prefix prefix;
        std::size_t wantSize;
    };
    std::vector<testCase> tests = {
        {mustPrefix("1.2.3.4/24"), 4 + 1},
        {mustPrefix("fd7a:115c:a1e0:ab12:4843:cd96:626b:430b/118"), 16 + 1},
        {mustPrefix("::ffff:c000:0280/96"), 16 + 1},
        {PrefixFrom(mustIP("::ffff:c000:0280").WithZone("eth0"), 37), 16 + 1},
    };
    tests.push_back({PrefixFrom(tests[0].prefix.Addr(), 33), tests[0].wantSize});
    tests.push_back({PrefixFrom(tests[1].prefix.Addr(), 129), tests[1].wantSize});
    for (auto& tc : tests) {
        auto [b, err] = tc.prefix.MarshalBinary();
        ASSERT_FALSE(err);
        ASSERT_EQ(b.size(), tc.wantSize) << tc.prefix;
        Prefix prefix2;
        ASSERT_FALSE(prefix2.UnmarshalBinary(b));
        EXPECT_EQ(tc.prefix, prefix2);
        auto [bAppend, err2] = tc.prefix.AppendBinary(slice<byte>::make(4, 32));
        ASSERT_FALSE(err2);
        bAppend = bAppend.Sub(4);
        ASSERT_EQ(bAppend.size(), tc.wantSize);
        Prefix prefix3;
        ASSERT_FALSE(prefix3.UnmarshalBinary(bAppend));
        EXPECT_EQ(tc.prefix, prefix3);
    }
    for (std::size_t n : {3, 6}) {
        Prefix prefix2;
        std::vector<byte> b(n, 1);
        EXPECT_TRUE(prefix2.UnmarshalBinary(b)) << n;
    }
}

TEST(GoNetip, AddrMarshalUnmarshal) {
    // Where text marshaling differs from ParseAddr and String: empty text
    // is the zero Addr, and back.
    Addr ip = mustIP("1.2.3.4");
    ASSERT_FALSE(ip.UnmarshalText({}));
    EXPECT_EQ(ip, Addr{});
    auto [b, err] = ip.MarshalText();
    ASSERT_FALSE(err);
    EXPECT_EQ(str(b), "");
}

TEST(GoNetip, AddrFrom16) {
    EXPECT_EQ(AddrFrom16({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), MkAddr(Mk128(0, 1), Z6noz));
    EXPECT_EQ(AddrFrom16({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4}),
              MkAddr(Mk128(0, 0xffff01020304), Z6noz));
}

TEST(GoNetip, IPProperties) {
    Addr nilIP, unicast4 = mustIP("192.0.2.1"), unicast6 = mustIP("2001:db8::1"),
                unicastZone6 = mustIP("2001:db8::1%eth0"), unicast6Unassigned = mustIP("4000::1"),
                multicast4 = mustIP("224.0.0.1"), multicast6 = mustIP("ff02::1"),
                multicastZone6 = mustIP("ff02::1%eth0"), llu4 = mustIP("169.254.0.1"), llu6 = mustIP("fe80::1"),
                llu6Last = mustIP("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), lluZone6 = mustIP("fe80::1%eth0"),
                loopback4 = mustIP("127.0.0.1"), ilm6 = mustIP("ff01::1"), ilmZone6 = mustIP("ff01::1%eth0"),
                private4a = mustIP("10.0.0.1"), private4b = mustIP("172.16.0.1"), private4c = mustIP("192.168.1.1"),
                private6 = mustIP("fd00::1"), private6mapped4a = mustIP("::ffff:10.0.0.1"),
                private6mapped4b = mustIP("::ffff:172.16.0.1"), private6mapped4c = mustIP("::ffff:192.168.1.1");
    struct {
        std::string name;
        Addr ip;
        bool globalUnicast = false, interfaceLocalMulticast = false, linkLocalMulticast = false,
             linkLocalUnicast = false, loopback = false, multicast = false, private_ = false, unspecified = false;
    } tests[] = {
        {.name = "nil", .ip = nilIP},
        {.name = "unicast v4Addr", .ip = unicast4, .globalUnicast = true},
        {.name = "unicast v6 mapped v4Addr", .ip = AddrFrom16(unicast4.As16()), .globalUnicast = true},
        {.name = "unicast v6Addr", .ip = unicast6, .globalUnicast = true},
        {.name = "unicast v6AddrZone", .ip = unicastZone6, .globalUnicast = true},
        {.name = "unicast v6Addr unassigned", .ip = unicast6Unassigned, .globalUnicast = true},
        {.name = "multicast v4Addr", .ip = multicast4, .linkLocalMulticast = true, .multicast = true},
        {.name = "multicast v6 mapped v4Addr",
         .ip = AddrFrom16(multicast4.As16()),
         .linkLocalMulticast = true,
         .multicast = true},
        {.name = "multicast v6Addr", .ip = multicast6, .linkLocalMulticast = true, .multicast = true},
        {.name = "multicast v6AddrZone", .ip = multicastZone6, .linkLocalMulticast = true, .multicast = true},
        {.name = "link-local unicast v4Addr", .ip = llu4, .linkLocalUnicast = true},
        {.name = "link-local unicast v6 mapped v4Addr", .ip = AddrFrom16(llu4.As16()), .linkLocalUnicast = true},
        {.name = "link-local unicast v6Addr", .ip = llu6, .linkLocalUnicast = true},
        {.name = "link-local unicast v6Addr upper bound", .ip = llu6Last, .linkLocalUnicast = true},
        {.name = "link-local unicast v6AddrZone", .ip = lluZone6, .linkLocalUnicast = true},
        {.name = "loopback v4Addr", .ip = loopback4, .loopback = true},
        {.name = "loopback v6Addr", .ip = IPv6Loopback(), .loopback = true},
        {.name = "loopback v6 mapped v4Addr", .ip = AddrFrom16(IPv6Loopback().As16()), .loopback = true},
        {.name = "interface-local multicast v6Addr", .ip = ilm6, .interfaceLocalMulticast = true, .multicast = true},
        {.name = "interface-local multicast v6AddrZone",
         .ip = ilmZone6,
         .interfaceLocalMulticast = true,
         .multicast = true},
        {.name = "private v4Addr 10/8", .ip = private4a, .globalUnicast = true, .private_ = true},
        {.name = "private v4Addr 172.16/12", .ip = private4b, .globalUnicast = true, .private_ = true},
        {.name = "private v4Addr 192.168/16", .ip = private4c, .globalUnicast = true, .private_ = true},
        {.name = "private v6Addr", .ip = private6, .globalUnicast = true, .private_ = true},
        {.name = "private v6 mapped v4Addr 10/8", .ip = private6mapped4a, .globalUnicast = true, .private_ = true},
        {.name = "private v6 mapped v4Addr 172.16/12",
         .ip = private6mapped4b,
         .globalUnicast = true,
         .private_ = true},
        {.name = "private v6 mapped v4Addr 192.168/16",
         .ip = private6mapped4c,
         .globalUnicast = true,
         .private_ = true},
        {.name = "unspecified v4Addr", .ip = IPv4Unspecified(), .unspecified = true},
        {.name = "unspecified v6Addr", .ip = IPv6Unspecified(), .unspecified = true},
    };
    for (auto& tt : tests) {
        SCOPED_TRACE(tt.name);
        EXPECT_EQ(tt.ip.IsGlobalUnicast(), tt.globalUnicast);
        EXPECT_EQ(tt.ip.IsInterfaceLocalMulticast(), tt.interfaceLocalMulticast);
        EXPECT_EQ(tt.ip.IsLinkLocalUnicast(), tt.linkLocalUnicast);
        EXPECT_EQ(tt.ip.IsLinkLocalMulticast(), tt.linkLocalMulticast);
        EXPECT_EQ(tt.ip.IsLoopback(), tt.loopback);
        EXPECT_EQ(tt.ip.IsMulticast(), tt.multicast);
        EXPECT_EQ(tt.ip.IsPrivate(), tt.private_);
        EXPECT_EQ(tt.ip.IsUnspecified(), tt.unspecified);
    }
}

TEST(GoNetip, AddrWellKnown) {
    struct {
        Addr ip;
        net::IP std_;
    } tests[] = {
        {IPv4Unspecified(), net::IPv4zero},
        {IPv6LinkLocalAllNodes(), net::IPv6linklocalallnodes},
        {IPv6LinkLocalAllRouters(), net::IPv6linklocalallrouters},
        {IPv6Loopback(), net::IPv6loopback},
        {IPv6Unspecified(), net::IPv6unspecified},
    };
    for (auto& tt : tests) EXPECT_EQ(str(tt.ip.String()), str(tt.std_.String()));
}

TEST(GoNetip, AddrLessCompare) {
    struct {
        Addr a, b;
        bool want;
    } tests[] = {
        {Addr{}, Addr{}, false},
        {Addr{}, mustIP("1.2.3.4"), true},
        {mustIP("1.2.3.4"), Addr{}, false},
        {mustIP("1.2.3.4"), mustIP("0102:0304::0"), true},
        {mustIP("0102:0304::0"), mustIP("1.2.3.4"), false},
        {mustIP("1.2.3.4"), mustIP("1.2.3.4"), false},
        {mustIP("::1"), mustIP("::2"), true},
        {mustIP("::1"), mustIP("::1%foo"), true},
        {mustIP("::1%foo"), mustIP("::2"), true},
        {mustIP("::2"), mustIP("::3"), true},
        {mustIP("::"), mustIP("0.0.0.0"), false},
        {mustIP("0.0.0.0"), mustIP("::"), true},
        {mustIP("::1%a"), mustIP("::1%b"), true},
        {mustIP("::1%a"), mustIP("::1%a"), false},
        {mustIP("::1%b"), mustIP("::1%a"), false},
        // go.dev/issue/68113: IPv4 and IPv4-mapped differ.
        {mustIP("::ffff:11.1.1.12"), mustIP("11.1.1.12"), false},
    };
    for (auto& tt : tests) {
        bool got = tt.a.Less(tt.b);
        EXPECT_EQ(got, tt.want) << tt.a << " < " << tt.b;
        int cmp = tt.a.Compare(tt.b);
        if (got) { EXPECT_EQ(cmp, -1); }
        EXPECT_TRUE(cmp >= -1 && cmp <= 1);
        if (cmp == 0) { EXPECT_EQ(tt.a, tt.b); }
        if (cmp == 1) { EXPECT_TRUE(tt.b.Less(tt.a)); }
        if (got == tt.want && got) { EXPECT_FALSE(tt.b.Less(tt.a)); }
    }
    std::vector<Addr> values = {mustIP("::1"),     mustIP("::2"),     Addr{},
                                mustIP("1.2.3.4"), mustIP("8.8.8.8"), mustIP("::1%foo")};
    std::sort(values.begin(), values.end(), [](const Addr& a, const Addr& b) { return a.Compare(b) < 0; });
    EXPECT_EQ(join(values), "[invalid IP 1.2.3.4 8.8.8.8 ::1 ::1%foo ::2]");
}

TEST(GoNetip, AddrPortCompare) {
    struct {
        AddrPort a, b;
        int want;
    } tests[] = {
        {AddrPort{}, AddrPort{}, 0},
        {AddrPort{}, mustIPPort("1.2.3.4:80"), -1},
        {mustIPPort("1.2.3.4:80"), mustIPPort("1.2.3.4:80"), 0},
        {mustIPPort("[::1]:80"), mustIPPort("[::1]:80"), 0},
        {mustIPPort("1.2.3.4:80"), mustIPPort("2.3.4.5:22"), -1},
        {mustIPPort("[::1]:80"), mustIPPort("[::2]:22"), -1},
        {mustIPPort("1.2.3.4:80"), mustIPPort("1.2.3.4:443"), -1},
        {mustIPPort("[::1]:80"), mustIPPort("[::1]:443"), -1},
        {mustIPPort("1.2.3.4:80"), mustIPPort("[0102:0304::0]:80"), -1},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(tt.a.Compare(tt.b), tt.want) << tt.a << " " << tt.b;
        EXPECT_EQ(tt.b.Compare(tt.a), -tt.want) << tt.b << " " << tt.a;
    }
    std::vector<AddrPort> values = {mustIPPort("[::1]:80"),    mustIPPort("[::2]:80"),     AddrPort{},
                                    mustIPPort("1.2.3.4:443"), mustIPPort("8.8.8.8:8080"), mustIPPort("[::1%foo]:1024")};
    std::sort(values.begin(), values.end(), [](auto& a, auto& b) { return a.Compare(b) < 0; });
    EXPECT_EQ(join(values), "[invalid AddrPort 1.2.3.4:443 8.8.8.8:8080 [::1]:80 [::1%foo]:1024 [::2]:80]");
}

TEST(GoNetip, PrefixCompare) {
    struct {
        Prefix a, b;
        int want;
    } tests[] = {
        {Prefix{}, Prefix{}, 0},
        {Prefix{}, mustPrefix("1.2.3.0/24"), -1},
        {mustPrefix("1.2.3.0/24"), mustPrefix("1.2.3.0/24"), 0},
        {mustPrefix("fe80::/64"), mustPrefix("fe80::/64"), 0},
        {mustPrefix("1.2.3.0/24"), mustPrefix("1.2.4.0/24"), -1},
        {mustPrefix("fe80::/64"), mustPrefix("fe90::/64"), -1},
        {mustPrefix("1.2.0.0/16"), mustPrefix("1.2.0.0/24"), -1},
        {mustPrefix("fe80::/48"), mustPrefix("fe80::/64"), -1},
        {mustPrefix("1.2.3.0/24"), mustPrefix("fe80::/8"), -1},
        {mustPrefix("1.2.3.0/24"), mustPrefix("1.2.3.4/24"), -1},
        {mustPrefix("1.2.3.0/24"), mustPrefix("1.2.3.0/28"), -1},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(tt.a.Compare(tt.b), tt.want) << tt.a << " " << tt.b;
        EXPECT_EQ(tt.b.Compare(tt.a), -tt.want) << tt.b << " " << tt.a;
    }
    auto sorted = [](std::vector<Prefix> v) {
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.Compare(b) < 0; });
        return join(v);
    };
    EXPECT_EQ(sorted({mustPrefix("1.2.3.0/24"), mustPrefix("fe90::/64"), mustPrefix("fe80::/64"),
                      mustPrefix("1.2.0.0/16"), Prefix{}, mustPrefix("fe80::/48"), mustPrefix("1.2.0.0/24"),
                      mustPrefix("1.2.3.4/24"), mustPrefix("1.2.3.0/28")}),
              "[invalid Prefix 1.2.0.0/16 1.2.0.0/24 1.2.3.0/24 1.2.3.4/24 1.2.3.0/28 fe80::/48 fe80::/64 "
              "fe90::/64]");
    // IANA's special-purpose registries, in their conventional order.
    std::vector<Prefix> values;
    for (auto s : {"0.0.0.0/8",      "127.0.0.0/8",     "10.0.0.0/8",         "203.0.113.0/24", "169.254.0.0/16",
                   "192.0.0.0/24",   "240.0.0.0/4",     "192.0.2.0/24",       "192.0.0.170/32", "198.18.0.0/15",
                   "192.0.0.8/32",   "0.0.0.0/32",      "192.0.0.9/32",       "198.51.100.0/24", "192.168.0.0/16",
                   "192.0.0.10/32",  "192.175.48.0/24", "192.52.193.0/24",    "100.64.0.0/10",  "255.255.255.255/32",
                   "192.31.196.0/24", "172.16.0.0/12",  "192.0.0.0/29",       "192.88.99.0/24", "fec0::/10",
                   "6000::/3",       "fe00::/9",        "8000::/3",           "0000::/8",       "0400::/6",
                   "f800::/6",       "e000::/4",        "ff00::/8",           "a000::/3",       "fc00::/7",
                   "1000::/4",       "0800::/5",        "4000::/3",           "0100::/8",       "c000::/3",
                   "fe80::/10",      "0200::/7",        "f000::/5",           "2000::/3"}) {
        values.push_back(mustPrefix(s));
    }
    EXPECT_EQ(sorted(values),
              "[0.0.0.0/8 0.0.0.0/32 10.0.0.0/8 100.64.0.0/10 127.0.0.0/8 169.254.0.0/16 172.16.0.0/12 192.0.0.0/24 "
              "192.0.0.0/29 192.0.0.8/32 192.0.0.9/32 192.0.0.10/32 192.0.0.170/32 192.0.2.0/24 192.31.196.0/24 "
              "192.52.193.0/24 192.88.99.0/24 192.168.0.0/16 192.175.48.0/24 198.18.0.0/15 198.51.100.0/24 "
              "203.0.113.0/24 240.0.0.0/4 255.255.255.255/32 ::/8 100::/8 200::/7 400::/6 800::/5 1000::/4 2000::/3 "
              "4000::/3 6000::/3 8000::/3 a000::/3 c000::/3 e000::/4 f000::/5 f800::/6 fc00::/7 fe00::/9 fe80::/10 "
              "fec0::/10 ff00::/8]");
}

TEST(GoNetip, IPStringExpanded) {
    struct {
        Addr ip;
        std::string s;
    } tests[] = {
        {Addr{}, "invalid IP"},
        {mustIP("192.0.2.1"), "192.0.2.1"},
        {mustIP("::ffff:192.0.2.1"), "0000:0000:0000:0000:0000:ffff:c000:0201"},
        {mustIP("2001:db8::1"), "2001:0db8:0000:0000:0000:0000:0000:0001"},
        {mustIP("2001:db8::1%eth0"), "2001:0db8:0000:0000:0000:0000:0000:0001%eth0"},
    };
    for (auto& tt : tests) EXPECT_EQ(str(tt.ip.StringExpanded()), tt.s);
}

TEST(GoNetip, PrefixMasking) {
    struct subtest {
        Addr ip;
        uint8 bits;
        Prefix p = {};
        bool ok = false;
    };
    auto makeIPv6 = [](std::string zone) -> std::vector<subtest> {
        if (!zone.empty()) zone = "%" + zone;
        return {
            {mustIP("2001:db8::1" + zone), 255},
            {mustIP("2001:db8::1" + zone), 32, mustPrefix("2001:db8::/32"), true},
            {mustIP("fe80::dead:beef:dead:beef" + zone), 96, mustPrefix("fe80::dead:beef:0:0/96"), true},
            {mustIP("aaaa::" + zone), 4, mustPrefix("a000::/4"), true},
            {mustIP("::" + zone), 63, mustPrefix("::/63"), true},
        };
    };
    struct {
        std::string family;
        std::vector<subtest> subtests;
    } tests[] = {
        {"nil", {{Addr{}, 255, {}, true}, {Addr{}, 16, {}, true}}},
        {"IPv4",
         {
             {mustIP("192.0.2.0"), 255},
             {mustIP("192.0.2.0"), 16, mustPrefix("192.0.0.0/16"), true},
             {mustIP("255.255.255.255"), 20, mustPrefix("255.255.240.0/20"), true},
             {mustIP("100.98.156.66"), 10, mustPrefix("100.64.0.0/10"), true},
         }},
        {"IPv6", makeIPv6("")},
        {"IPv6 zone", makeIPv6("eth0")},
    };
    for (auto& tt : tests) {
        for (auto& st : tt.subtests) {
            SCOPED_TRACE(tt.family + " " + str(st.p.String()));
            string orig = st.ip.String();
            auto [p, err] = st.ip.Prefix(st.bits);
            if (st.ok) { ASSERT_FALSE(err) << err; }
            if (!st.ok) { ASSERT_TRUE(err); }
            if (err) continue;
            EXPECT_EQ(p, st.p);
            EXPECT_EQ(st.ip.String(), orig);
        }
    }
}

TEST(GoNetip, PrefixMarshalUnmarshal) {
    for (std::string s : {"", "1.2.3.4/32", "0.0.0.0/0", "::/0", "::1/128", "2001:db8::/32"}) {
        Prefix p;
        ASSERT_FALSE(p.UnmarshalText(as_bytes(s))) << s;
        auto [b, err] = p.MarshalText();
        ASSERT_FALSE(err);
        EXPECT_EQ(str(b), s);
    }
}

TEST(GoNetip, PrefixUnmarshalTextNonZero) {
    Prefix ip = mustPrefix("fe80::/64");
    EXPECT_TRUE(ip.UnmarshalText(as_bytes("xxx")));
}

TEST(GoNetip, Is4AndIs6) {
    struct {
        Addr ip;
        bool is4, is6;
    } tests[] = {
        {Addr{}, false, false},
        {mustIP("1.2.3.4"), true, false},
        {mustIP("127.0.0.2"), true, false},
        {mustIP("::1"), false, true},
        {mustIP("::ffff:192.0.2.128"), false, true},
        {mustIP("::fffe:c000:0280"), false, true},
        {mustIP("::1%eth0"), false, true},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(tt.ip.Is4(), tt.is4) << tt.ip;
        EXPECT_EQ(tt.ip.Is6(), tt.is6) << tt.ip;
    }
}

TEST(GoNetip, Is4In6) {
    struct {
        Addr ip;
        bool want;
        Addr wantUnmap;
    } tests[] = {
        {Addr{}, false, Addr{}},
        {mustIP("::ffff:c000:0280"), true, mustIP("192.0.2.128")},
        {mustIP("::ffff:192.0.2.128"), true, mustIP("192.0.2.128")},
        {mustIP("::ffff:192.0.2.128%eth0"), true, mustIP("192.0.2.128")},
        {mustIP("::fffe:c000:0280"), false, mustIP("::fffe:c000:0280")},
        {mustIP("::ffff:127.1.2.3"), true, mustIP("127.1.2.3")},
        {mustIP("::ffff:7f01:0203"), true, mustIP("127.1.2.3")},
        {mustIP("0:0:0:0:0000:ffff:127.1.2.3"), true, mustIP("127.1.2.3")},
        {mustIP("0:0:0:0::ffff:127.1.2.3"), true, mustIP("127.1.2.3")},
        {mustIP("::1"), false, mustIP("::1")},
        {mustIP("1.2.3.4"), false, mustIP("1.2.3.4")},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(tt.ip.Is4In6(), tt.want) << tt.ip;
        EXPECT_EQ(tt.ip.Unmap(), tt.wantUnmap) << tt.ip;
    }
}

TEST(GoNetip, PrefixMasked) {
    struct {
        Prefix prefix, masked;
    } tests[] = {
        {mustPrefix("192.168.0.255/24"), mustPrefix("192.168.0.0/24")},
        {mustPrefix("2100::/3"), mustPrefix("2000::/3")},
        {PrefixFrom(mustIP("2000::"), 129), Prefix{}},
        {PrefixFrom(mustIP("1.2.3.4"), 33), Prefix{}},
    };
    for (auto& test : tests) EXPECT_EQ(test.prefix.Masked(), test.masked) << test.prefix;
}

TEST(GoNetip, Prefix) {
    struct {
        std::string prefix;
        Addr ip;
        int bits;
        std::string str_;
        std::vector<Addr> contains, notContains;
    } tests[] = {
        {"192.168.0.0/24", mustIP("192.168.0.0"), 24, "", mustIPs({"192.168.0.1", "192.168.0.55"}),
         mustIPs({"192.168.1.1", "1.1.1.1"})},
        {"192.168.1.1/32", mustIP("192.168.1.1"), 32, "", mustIPs({"192.168.1.1"}), mustIPs({"192.168.1.2"})},
        {"100.64.0.0/10", mustIP("100.64.0.0"), 10, "",
         mustIPs({"100.64.0.0", "100.64.0.1", "100.81.251.94", "100.100.100.100", "100.127.255.254",
                  "100.127.255.255"}),
         mustIPs({"100.63.255.255", "100.128.0.0"})},
        {"2001:db8::/96", mustIP("2001:db8::"), 96, "", mustIPs({"2001:db8::aaaa:bbbb", "2001:db8::1"}),
         mustIPs({"2001:db8::1:aaaa:bbbb", "2001:db9::"})},
        {"0.0.0.0/0", mustIP("0.0.0.0"), 0, "", mustIPs({"192.168.0.1", "1.1.1.1"}),
         {mustIP("2001:db8::1"), Addr{}}},
        {"::/0", mustIP("::"), 0, "", mustIPs({"::1", "2001:db8::1"}), mustIPs({"192.0.2.1"})},
        {"2000::/3", mustIP("2000::"), 3, "", mustIPs({"2001:db8::1"}), mustIPs({"fe80::1"})},
    };
    for (auto& test : tests) {
        SCOPED_TRACE(test.prefix);
        auto [prefix, err] = ParsePrefix(test.prefix);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(prefix.Addr(), test.ip);
        EXPECT_EQ(prefix.Bits(), test.bits);
        for (auto& ip : test.contains) EXPECT_TRUE(prefix.Contains(ip)) << ip;
        for (auto& ip : test.notContains) EXPECT_FALSE(prefix.Contains(ip)) << ip;
        EXPECT_EQ(str(prefix.String()), test.str_.empty() ? test.prefix : test.str_);
        TestAppendToMarshal(prefix);
    }
}

TEST(GoNetip, PrefixFromInvalidBits) {
    Addr v4 = MustParseAddr("1.2.3.4"), v6 = MustParseAddr("66::66");
    struct {
        Addr ip;
        int in, want;
    } tests[] = {
        {v4, 0, 0},     {v6, 0, 0},     {v4, 1, 1},     {v4, 33, -1},   {v6, 33, 33},
        {v6, 127, 127}, {v6, 128, 128}, {v4, 254, -1},  {v4, 255, -1},  {v4, -1, -1},
        {v6, -1, -1},   {v4, -5, -1},   {v6, -5, -1},
    };
    for (auto& tt : tests) EXPECT_EQ(PrefixFrom(tt.ip, tt.in).Bits(), tt.want) << tt.ip << " " << tt.in;
}

TEST(GoNetip, ParsePrefixError) {
    struct {
        std::string prefix, errstr;
    } tests[] = {
        {"192.168.0.0", "no '/'"},
        {"1.257.1.1/24", "value >255"},
        {"1.1.1.0/q", "bad bits"},
        {"1.1.1.0/-1", "bad bits"},
        {"1.1.1.0/33", "out of range"},
        {"2001::/129", "out of range"},
        {"1.1.1.0%a/24", "unexpected character"},
        {"2001:db8::%a/32", "zones cannot be present"},
        {"1.1.1.0/+32", "bad bits"},
        {"1.1.1.0/-32", "bad bits"},
        {"1.1.1.0/032", "bad bits"},
        {"1.1.1.0/0032", "bad bits"},
    };
    for (auto& test : tests) {
        auto [p, err] = ParsePrefix(test.prefix);
        ASSERT_TRUE(err) << test.prefix;
        EXPECT_NE(str(err.Error()).find(test.errstr), std::string::npos) << err;
    }
}

TEST(GoNetip, PrefixIsSingleIP) {
    struct {
        Prefix ipp;
        bool want;
    } tests[] = {
        {mustPrefix("127.0.0.1/32"), true}, {mustPrefix("127.0.0.1/31"), false}, {mustPrefix("127.0.0.1/0"), false},
        {mustPrefix("::1/128"), true},      {mustPrefix("::1/127"), false},      {mustPrefix("::1/0"), false},
        {Prefix{}, false},
    };
    for (auto& tt : tests) EXPECT_EQ(tt.ipp.IsSingleIP(), tt.want) << tt.ipp;
}

TEST(GoNetip, As4) {
    EXPECT_EQ(mustIP("1.2.3.4").As4(), (std::array<byte, 4>{1, 2, 3, 4}));
    EXPECT_EQ(AddrFrom16(mustIP("1.2.3.4").As16()).As4(), (std::array<byte, 4>{1, 2, 3, 4}));
    EXPECT_EQ(mustIP("0.0.0.0").As4(), (std::array<byte, 4>{0, 0, 0, 0}));
}

TEST(GoNetip, PrefixOverlaps) {
    auto pfx = mustPrefix;
    struct {
        Prefix a, b;
        bool want;
    } tests[] = {
        {Prefix{}, pfx("1.2.0.0/16"), false},
        {pfx("1.2.0.0/16"), Prefix{}, false},
        {pfx("::0/3"), pfx("0.0.0.0/3"), false},
        {pfx("1.2.0.0/16"), pfx("1.2.0.0/16"), true},
        {pfx("1.2.0.0/16"), pfx("1.2.3.0/24"), true},
        {pfx("1.2.3.0/24"), pfx("1.2.0.0/16"), true},
        {pfx("1.2.0.0/16"), pfx("1.2.3.0/32"), true},
        {pfx("1.2.3.0/32"), pfx("1.2.0.0/16"), true},
        {pfx("1.2.3.0/32"), pfx("0.0.0.0/0"), true},
        {pfx("0.0.0.0/0"), pfx("1.2.3.0/32"), true},
        {pfx("1.2.3.0/32"), pfx("5.5.5.5/0"), true},
        {pfx("5::1/128"), pfx("5::0/8"), true},
        {pfx("5::0/8"), pfx("5::1/128"), true},
        {pfx("1::1/128"), pfx("2::2/128"), false},
        {pfx("0100::0/8"), pfx("::1/128"), false},
        {PrefixFrom(AddrFrom16(mustIP("1.2.0.0").As16()), 16), pfx("1.2.3.0/24"), false},
        {PrefixFrom(mustIP("1.2.3.4"), 33), pfx("1.2.3.0/24"), false},
        {PrefixFrom(mustIP("2000::"), 129), pfx("2000::/64"), false},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(tt.a.Overlaps(tt.b), tt.want) << tt.a << " " << tt.b;
        EXPECT_EQ(tt.b.Overlaps(tt.a), tt.want) << tt.b << " " << tt.a;
    }
}

TEST(GoNetip, PrefixString) {
    EXPECT_EQ(str(Prefix{}.String()), "invalid Prefix");
    EXPECT_EQ(str(PrefixFrom(Addr{}, 8).String()), "invalid Prefix");
    EXPECT_EQ(str(PrefixFrom(MustParseAddr("1.2.3.4"), 88).String()), "invalid Prefix");
}

TEST(GoNetip, AddrPortString) {
    EXPECT_EQ(str(MustParseAddrPort("127.0.0.1:80").String()), "127.0.0.1:80");
    EXPECT_EQ(str(MustParseAddrPort("[0000::0]:8080").String()), "[::]:8080");
    EXPECT_EQ(str(MustParseAddrPort("[FFFF::1]:8080").String()), "[ffff::1]:8080");
    EXPECT_EQ(str(AddrPort{}.String()), "invalid AddrPort");
    EXPECT_EQ(str(AddrPortFrom(Addr{}, 80).String()), "invalid AddrPort");
}

TEST(GoNetip, AsSlice) {
    EXPECT_TRUE(Addr{}.AsSlice().isNil());
    EXPECT_EQ(str(mustIP("1.2.3.4").AsSlice()), std::string("\x01\x02\x03\x04", 4));
    std::string want(16, '\0');
    want[0] = want[1] = '\xff';
    want[15] = 1;
    EXPECT_EQ(str(mustIP("ffff::1").AsSlice()), want);
}

// --- netip_pkg_test.go --------------------------------------------------

TEST(GoNetip, PrefixValid) {
    Addr v4 = MustParseAddr("1.2.3.4"), v6 = MustParseAddr("::1");
    struct {
        Prefix ipp;
        bool want;
    } tests[] = {
        {PrefixFrom(v4, -2), false},     {PrefixFrom(v4, -1), false},     {PrefixFrom(v4, 0), true},
        {PrefixFrom(v4, 32), true},      {PrefixFrom(v4, 33), false},     {PrefixFrom(v6, -2), false},
        {PrefixFrom(v6, -1), false},     {PrefixFrom(v6, 0), true},       {PrefixFrom(v6, 32), true},
        {PrefixFrom(v6, 128), true},     {PrefixFrom(v6, 129), false},    {PrefixFrom(Addr{}, -2), false},
        {PrefixFrom(Addr{}, -1), false}, {PrefixFrom(Addr{}, 0), false},  {PrefixFrom(Addr{}, 32), false},
        {PrefixFrom(Addr{}, 128), false},
    };
    for (auto& tt : tests) {
        bool got = tt.ipp.IsValid();
        EXPECT_EQ(got, tt.want) << tt.ipp;
        // Only one invalid Prefix per Addr.
        if (!got) { EXPECT_EQ(tt.ipp, PrefixFrom(tt.ipp.Addr(), -1)); }
    }
}

TEST(GoNetip, IPNextPrev) {
    struct {
        Addr ip, next, prev;
    } nextPrevTests[] = {
        {mustIP("10.0.0.1"), mustIP("10.0.0.2"), mustIP("10.0.0.0")},
        {mustIP("10.0.0.255"), mustIP("10.0.1.0"), mustIP("10.0.0.254")},
        {mustIP("127.0.0.1"), mustIP("127.0.0.2"), mustIP("127.0.0.0")},
        {mustIP("254.255.255.255"), mustIP("255.0.0.0"), mustIP("254.255.255.254")},
        {mustIP("255.255.255.255"), Addr{}, mustIP("255.255.255.254")},
        {mustIP("0.0.0.0"), mustIP("0.0.0.1"), Addr{}},
        {mustIP("::"), mustIP("::1"), Addr{}},
        {mustIP("::%x"), mustIP("::1%x"), Addr{}},
        {mustIP("::1"), mustIP("::2"), mustIP("::")},
        {mustIP("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), Addr{}, mustIP("ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe")},
    };
    for (auto& tt : nextPrevTests) {
        EXPECT_EQ(tt.ip.Next(), tt.next) << tt.ip;
        EXPECT_EQ(tt.ip.Prev(), tt.prev) << tt.ip;
        if (!tt.ip.Next().isZero()) { EXPECT_EQ(tt.ip.Next().Prev(), tt.ip); }
        if (!tt.ip.Prev().isZero()) { EXPECT_EQ(tt.ip.Prev().Next(), tt.ip); }
    }
    for (auto ip : {mustIP("0.0.0.0"), mustIP("::")}) EXPECT_TRUE(ip.Prev().isZero()) << ip;
    std::array<byte, 16> allFF;
    allFF.fill(0xff);
    for (auto ip : {mustIP("255.255.255.255"), AddrFrom16(allFF)}) EXPECT_TRUE(ip.Next().isZero()) << ip;
}

TEST(GoNetip, IPBitLen) {
    EXPECT_EQ(Addr{}.BitLen(), 0);
    EXPECT_EQ(mustIP("0.0.0.0").BitLen(), 32);
    EXPECT_EQ(mustIP("10.0.0.1").BitLen(), 32);
    EXPECT_EQ(mustIP("::").BitLen(), 128);
    EXPECT_EQ(mustIP("fed0::1").BitLen(), 128);
    EXPECT_EQ(mustIP("::ffff:10.0.0.1").BitLen(), 128);
}

TEST(GoNetip, PrefixContains) {
    struct {
        Prefix ipp;
        Addr ip;
        bool want;
    } tests[] = {
        {mustPrefix("9.8.7.6/0"), mustIP("9.8.7.6"), true},
        {mustPrefix("9.8.7.6/16"), mustIP("9.8.7.6"), true},
        {mustPrefix("9.8.7.6/16"), mustIP("9.8.6.4"), true},
        {mustPrefix("9.8.7.6/16"), mustIP("9.9.7.6"), false},
        {mustPrefix("9.8.7.6/32"), mustIP("9.8.7.6"), true},
        {mustPrefix("9.8.7.6/32"), mustIP("9.8.7.7"), false},
        {mustPrefix("9.8.7.6/32"), mustIP("9.8.7.7"), false},
        {mustPrefix("::1/0"), mustIP("::1"), true},
        {mustPrefix("::1/0"), mustIP("::2"), true},
        {mustPrefix("::1/127"), mustIP("::1"), true},
        {mustPrefix("::1/127"), mustIP("::2"), false},
        {mustPrefix("::1/128"), mustIP("::1"), true},
        {mustPrefix("::1/127"), mustIP("::2"), false},
        // Zones are ignored: go.dev/issue/51899.
        {Prefix(mustIP("1.2.3.4").WithZone("a"), 32), mustIP("1.2.3.4"), true},
        {Prefix(mustIP("::1").WithZone("a"), 128), mustIP("::1"), true},
        {mustPrefix("::1/0"), Addr{}, false},
        {mustPrefix("1.2.3.4/0"), Addr{}, false},
        {PrefixFrom(mustIP("::1"), 129), mustIP("::1"), false},
        {PrefixFrom(mustIP("1.2.3.4"), 33), mustIP("1.2.3.4"), false},
        {PrefixFrom(Addr{}, 0), mustIP("1.2.3.4"), false},
        {PrefixFrom(Addr{}, 32), mustIP("1.2.3.4"), false},
        {PrefixFrom(Addr{}, 128), mustIP("::1"), false},
        {mustPrefix("::1/0"), mustIP("1.2.3.4"), false},
        {mustPrefix("1.2.3.4/0"), mustIP("::1"), false},
    };
    for (auto& tt : tests) EXPECT_EQ(tt.ipp.Contains(tt.ip), tt.want) << tt.ipp << " " << tt.ip;
}

TEST(GoNetip, ParseIPError) {
    struct {
        std::string ip, errstr;
    } tests[] = {
        {"localhost", ""},
        {"500.0.0.1", "field has value >255"},
        {"::gggg%eth0", "must have at least one digit"},
        {"fe80::1cc0:3e8c:119f:c2e1%", "zone must be a non-empty string"},
        {"%eth0", "missing IPv6 address"},
    };
    for (auto& test : tests) {
        auto [ip, err] = ParseAddr(test.ip);
        ASSERT_TRUE(err) << test.ip;
        std::string want = test.errstr.empty() ? "unable to parse IP" : test.errstr;
        EXPECT_NE(str(err.Error()).find(want), std::string::npos) << err;
    }
}

TEST(GoNetip, ParseAddrPort) {
    struct {
        std::string in;
        AddrPort want;
        bool wantErr = false;
    } tests[] = {
        {"1.2.3.4:1234", AddrPort(mustIP("1.2.3.4"), 1234)},
        {"1.1.1.1:123456", {}, true},
        {"1.1.1.1:-123", {}, true},
        {"[::1]:1234", AddrPort(mustIP("::1"), 1234)},
        {"[1.2.3.4]:1234", {}, true},
        {"fe80::1:1234", {}, true},
        {":0", {}, true},
    };
    for (auto& test : tests) {
        SCOPED_TRACE(test.in);
        auto [got, err] = ParseAddrPort(test.in);
        if (err) {
            EXPECT_TRUE(test.wantErr) << err;
        } else {
            EXPECT_FALSE(test.wantErr);
            EXPECT_EQ(got, test.want);
            EXPECT_EQ(str(got.String()), test.in);
            TestAppendToMarshal(got);
        }
        AddrPort tgot;
        error terr = tgot.UnmarshalText(as_bytes(test.in));
        EXPECT_EQ(bool(terr), test.wantErr);
        if (!terr) {
            EXPECT_EQ(tgot, test.want);
            EXPECT_EQ(str(tgot.MarshalText().first), test.in);
        }
    }
}

TEST(GoNetip, AddrPortMarshalUnmarshal) {
    AddrPort ipp = mustIPPort("1.2.3.4:80");
    ASSERT_FALSE(ipp.UnmarshalText({}));
    EXPECT_EQ(ipp, AddrPort{});
    EXPECT_EQ(str(ipp.MarshalText().first), "");
    TestAppendToMarshal(ipp);
}

TEST(GoNetip, IPv6Accessor) {
    std::array<byte, 16> a;
    for (int i = 0; i < 16; i++) a[i] = byte(i + 1);
    Addr ip = AddrFrom16(a);
    for (int i = 0; i < 16; i++) EXPECT_EQ(ip.v6(uint8(i)), uint8(i + 1)) << i;
}

// --- uint128_test.go ----------------------------------------------------

TEST(GoNetip, Uint128AddSub) {
    constexpr uint64 ff = ~uint64(0);
    struct {
        uint128 in;
        int op;
        uint128 want;
    } tests[] = {
        {{0, 0}, 1, {0, 1}},   {{0, 1}, 1, {0, 2}},   {{1, 0}, 1, {1, 1}},   {{0, ff}, 1, {1, 0}},
        {{ff, ff}, 1, {0, 0}}, {{0, 0}, -1, {ff, ff}}, {{0, 1}, -1, {0, 0}}, {{0, 2}, -1, {0, 1}},
        {{1, 0}, -1, {0, ff}}, {{1, 1}, -1, {1, 0}},
    };
    for (auto& tt : tests) EXPECT_EQ(tt.op == 1 ? tt.in.addOne() : tt.in.subOne(), tt.want) << tt.in;
}

TEST(GoNetip, BitsSetFrom) {
    constexpr uint64 ff = ~uint64(0);
    struct {
        uint8 bit;
        uint128 want;
    } tests[] = {
        {0, {ff, ff}}, {1, {ff >> 1, ff}}, {63, {1, ff}}, {64, {0, ff}}, {65, {0, ff >> 1}}, {127, {0, 1}}, {128, {0, 0}},
    };
    for (auto& tt : tests) EXPECT_EQ(uint128{}.bitsSetFrom(tt.bit), tt.want) << int(tt.bit);
}

TEST(GoNetip, BitsClearedFrom) {
    constexpr uint64 ff = ~uint64(0);
    struct {
        uint8 bit;
        uint128 want;
    } tests[] = {
        {0, {0, 0}},         {1, {uint64(1) << 63, 0}},          {63, {ff & ~uint64(1), 0}}, {64, {ff, 0}},
        {65, {ff, uint64(1) << 63}}, {127, {ff, ff & ~uint64(1)}}, {128, {ff, ff}},
    };
    for (auto& tt : tests) EXPECT_EQ((uint128{ff, ff}).bitsClearedFrom(tt.bit), tt.want) << int(tt.bit);
}
