// IP addresses, after Go's net/ip.go, with the parsing and formatting of
// net/netip.
#include <gopp/net.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <optional>
#include <string>

namespace gopp::net {

namespace {

constexpr byte v4InV6Prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
constexpr char hexDigit[] = "0123456789abcdef";

bool equal(std::span<const byte> a, std::span<const byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool isZeros(std::span<const byte> p) {
    return std::all_of(p.begin(), p.end(), [](byte b) { return b == 0; });
}

bool allFF(std::span<const byte> b) {
    return std::all_of(b.begin(), b.end(), [](byte c) { return c == 0xff; });
}

std::string hexString(std::span<const byte> b) {
    std::string s(b.size() * 2, '0');
    for (std::size_t i = 0; i < b.size(); i++) {
        s[i * 2] = hexDigit[b[i] >> 4];
        s[i * 2 + 1] = hexDigit[b[i] & 0xf];
    }
    return s;
}

IP makeIp(std::initializer_list<byte> b) { return IP(b); }

// netip's parseIPv4Fields: four decimal fields without leading zeros.
bool parseIPv4Fields(std::string_view s, byte* fields) {
    int val = 0, pos = 0, digLen = 0;
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (digLen == 1 && val == 0) return false;  // leading zero
            val = val * 10 + (c - '0');
            digLen++;
            if (val > 255) return false;
        } else if (c == '.') {
            if (i == 0 || i == s.size() - 1 || s[i - 1] == '.') return false;
            if (pos == 3) return false;
            fields[pos++] = static_cast<byte>(val);
            val = 0;
            digLen = 0;
        } else {
            return false;
        }
    }
    if (pos < 3) return false;
    fields[3] = static_cast<byte>(val);
    return true;
}

// netip's parseIPv6, without zones (ParseIP rejects them).
bool parseIPv6(std::string_view in, byte* ip) {
    std::string_view s = in;
    std::memset(ip, 0, 16);
    int ellipsis = -1;
    if (s.size() >= 2 && s[0] == ':' && s[1] == ':') {
        ellipsis = 0;
        s.remove_prefix(2);
        if (s.empty()) return true;
    }
    int i = 0;
    while (i < 16) {
        std::size_t off = 0;
        uint32_t acc = 0;
        for (; off < s.size(); off++) {
            char c = s[off];
            if (c >= '0' && c <= '9') acc = (acc << 4) + uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') acc = (acc << 4) + uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') acc = (acc << 4) + uint32_t(c - 'A' + 10);
            else break;
            if (off > 3) return false;
            if (acc > 0xffff) return false;
        }
        if (off == 0) return false;
        if (off < s.size() && s[off] == '.') {
            // An embedded IPv4 address replaces the last two fields.
            if (ellipsis < 0 && i != 12) return false;
            if (i + 4 > 16) return false;
            if (!parseIPv4Fields(s, ip + i)) return false;
            s = {};
            i += 4;
            break;
        }
        ip[i] = static_cast<byte>(acc >> 8);
        ip[i + 1] = static_cast<byte>(acc);
        i += 2;
        s.remove_prefix(off);
        if (s.empty()) break;
        if (s[0] != ':' || s.size() == 1) return false;
        s.remove_prefix(1);
        if (s[0] == ':') {
            if (ellipsis >= 0) return false;
            ellipsis = i;
            s.remove_prefix(1);
            if (s.empty()) break;
        }
    }
    if (!s.empty()) return false;
    if (i < 16) {
        if (ellipsis < 0) return false;
        int n = 16 - i;
        for (int j = i - 1; j >= ellipsis; j--) ip[j + n] = ip[j];
        std::memset(ip + ellipsis, 0, static_cast<std::size_t>(n));
    } else if (ellipsis >= 0) {
        return false;  // an ellipsis must stand for at least one field
    }
    return true;
}

// The 16 bytes of s, an IPv4 or IPv6 address without zone.
std::optional<std::array<byte, 16>> parseIP(std::string_view s) {
    std::array<byte, 16> ip{};
    for (char c : s) {
        if (c == '.') {
            std::memcpy(ip.data(), v4InV6Prefix, 12);
            if (!parseIPv4Fields(s, ip.data() + 12)) return std::nullopt;
            return ip;
        }
        if (c == ':') {
            if (s.find('%') != s.npos || !parseIPv6(s, ip.data())) return std::nullopt;
            return ip;
        }
        if (c == '%') return std::nullopt;
    }
    return std::nullopt;
}

void appendDecimal(std::string& s, unsigned v) { s += std::to_string(v); }

// netip's appendTo6: RFC 5952, eliding the longest run of two or more zero
// fields, the first if there are several.
void appendTo6(std::string& s, std::span<const byte> ip) {
    auto field = [&](int i) { return unsigned(ip[2 * i]) << 8 | ip[2 * i + 1]; };
    int zeroStart = 255, zeroEnd = 255;
    for (int i = 0; i < 8; i++) {
        int j = i;
        while (j < 8 && field(j) == 0) j++;
        if (int l = j - i; l >= 2 && l > zeroEnd - zeroStart) {
            zeroStart = i;
            zeroEnd = j;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (i == zeroStart) {
            s += "::";
            i = zeroEnd;
            if (i >= 8) break;
        } else if (i > 0) {
            s += ':';
        }
        char buf[8];
        auto r = std::to_chars(buf, buf + sizeof(buf), field(i), 16);
        s.append(buf, r.ptr);
    }
}

void appendTo(std::string& s, const IP& ip0) {
    IP ip = ip0;
    if (IP p4 = ip.To4(); p4.size() == IPv4len) ip = p4;
    if (ip.size() == IPv4len) {
        for (int i = 0; i < 4; i++) {
            if (i) s += '.';
            appendDecimal(s, ip[i]);
        }
        return;
    }
    appendTo6(s, ip);
}

int simpleMaskLength(std::span<const byte> mask) {
    int n = 0;
    for (std::size_t i = 0; i < mask.size(); i++) {
        byte v = mask[i];
        if (v == 0xff) {
            n += 8;
            continue;
        }
        // Found a non-ff byte: count its ones, then the rest must be zero.
        while (v & 0x80) {
            n++;
            v = static_cast<byte>(v << 1);
        }
        if (v != 0) return -1;
        for (i++; i < mask.size(); i++) {
            if (mask[i] != 0) return -1;
        }
        break;
    }
    return n;
}

std::pair<IP, IPMask> networkNumberAndMask(const IPNet& n) {
    IP ip = n.IP.To4();
    if (ip.empty()) {
        ip = n.IP;
        if (ip.size() != IPv6len) return {};
    }
    IPMask m = n.Mask;
    switch (m.size()) {
    case IPv4len:
        if (ip.size() != IPv4len) return {};
        break;
    case IPv6len:
        if (ip.size() == IPv4len) m = m.Sub(12);
        break;
    default: return {};
    }
    return {ip, m};
}

}  // namespace

const IP IPv4bcast = IPv4(255, 255, 255, 255);
const IP IPv4allsys = IPv4(224, 0, 0, 1);
const IP IPv4allrouter = IPv4(224, 0, 0, 2);
const IP IPv4zero = IPv4(0, 0, 0, 0);
const IP IPv6zero = makeIp({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
const IP IPv6unspecified = makeIp({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
const IP IPv6loopback = makeIp({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
const IP IPv6interfacelocalallnodes = makeIp({0xff, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01});
const IP IPv6linklocalallnodes = makeIp({0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01});
const IP IPv6linklocalallrouters = makeIp({0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02});

IP IPv4(byte a, byte b, byte c, byte d) {
    IP p = IP::make(IPv6len);
    std::memcpy(p.data(), v4InV6Prefix, 12);
    p[12] = a;
    p[13] = b;
    p[14] = c;
    p[15] = d;
    return p;
}

IPMask IPv4Mask(byte a, byte b, byte c, byte d) { return IPMask{a, b, c, d}; }

IPMask CIDRMask(int ones, int bits) {
    if (bits != 8 * IPv4len && bits != 8 * IPv6len) return {};
    if (ones < 0 || ones > bits) return {};
    std::size_t l = static_cast<std::size_t>(bits / 8);
    IPMask m = IPMask::make(l);
    unsigned n = static_cast<unsigned>(ones);
    for (std::size_t i = 0; i < l; i++) {
        if (n >= 8) {
            m[i] = 0xff;
            n -= 8;
            continue;
        }
        m[i] = static_cast<byte>(~(0xffu >> n));
        n = 0;
    }
    return m;
}

IP IP::To4() const {
    if (size() == IPv4len) return *this;
    if (size() == IPv6len && isZeros(span().first(10)) && (*this)[10] == 0xff && (*this)[11] == 0xff) return Sub(12, 16);
    return {};
}

IP IP::To16() const {
    if (size() == IPv4len) return IPv4((*this)[0], (*this)[1], (*this)[2], (*this)[3]);
    if (size() == IPv6len) return *this;
    return {};
}

bool IP::Equal(const IP& x) const {
    if (size() == x.size()) return equal(span(), x.span());
    if (size() == IPv4len && x.size() == IPv6len) {
        return equal(x.span().first(12), v4InV6Prefix) && equal(span(), x.span().subspan(12));
    }
    if (size() == IPv6len && x.size() == IPv4len) {
        return equal(span().first(12), v4InV6Prefix) && equal(span().subspan(12), x.span());
    }
    return false;
}

slice<byte> IP::DefaultMask() const {
    IP ip = To4();
    if (ip.empty()) return {};
    if (ip[0] < 0x80) return IPv4Mask(0xff, 0, 0, 0);
    if (ip[0] < 0xc0) return IPv4Mask(0xff, 0xff, 0, 0);
    return IPv4Mask(0xff, 0xff, 0xff, 0);
}

IP IP::Mask(std::span<const byte> mask) const {
    IP ip = *this;
    if (mask.size() == IPv6len && ip.size() == IPv4len && allFF(mask.first(12))) mask = mask.subspan(12);
    if (mask.size() == IPv4len && ip.size() == IPv6len && equal(ip.span().first(12), v4InV6Prefix)) ip = ip.Sub(12);
    std::size_t n = ip.size();
    if (n != mask.size()) return {};
    IP out = IP::make(n);
    for (std::size_t i = 0; i < n; i++) out[i] = ip[i] & mask[i];
    return out;
}

bool IP::IsUnspecified() const { return Equal(IPv4zero) || Equal(IPv6unspecified); }

bool IP::IsLoopback() const {
    if (IP ip4 = To4(); !ip4.empty()) return ip4[0] == 127;
    return Equal(IPv6loopback);
}

bool IP::IsPrivate() const {
    if (IP ip4 = To4(); !ip4.empty()) {
        return ip4[0] == 10 || (ip4[0] == 172 && (ip4[1] & 0xf0) == 16) || (ip4[0] == 192 && ip4[1] == 168);
    }
    return size() == IPv6len && ((*this)[0] & 0xfe) == 0xfc;
}

bool IP::IsMulticast() const {
    if (IP ip4 = To4(); !ip4.empty()) return (ip4[0] & 0xf0) == 0xe0;
    return size() == IPv6len && (*this)[0] == 0xff;
}

bool IP::IsInterfaceLocalMulticast() const {
    return size() == IPv6len && (*this)[0] == 0xff && ((*this)[1] & 0x0f) == 0x01;
}

bool IP::IsLinkLocalMulticast() const {
    if (IP ip4 = To4(); !ip4.empty()) return ip4[0] == 224 && ip4[1] == 0 && ip4[2] == 0;
    return size() == IPv6len && (*this)[0] == 0xff && ((*this)[1] & 0x0f) == 0x02;
}

bool IP::IsLinkLocalUnicast() const {
    if (IP ip4 = To4(); !ip4.empty()) return ip4[0] == 169 && ip4[1] == 254;
    return size() == IPv6len && (*this)[0] == 0xfe && ((*this)[1] & 0xc0) == 0x80;
}

bool IP::IsGlobalUnicast() const {
    return (size() == IPv4len || size() == IPv6len) && !Equal(IPv4bcast) && !IsUnspecified() && !IsLoopback() &&
           !IsMulticast() && !IsLinkLocalUnicast();
}

string IP::String() const {
    if (empty()) return string::fromStatic("<nil>");
    if (size() != IPv4len && size() != IPv6len) return string("?" + hexString(span()));
    std::string s;
    appendTo(s, *this);
    return string(s);
}

std::pair<slice<byte>, error> IP::AppendText(slice<byte> b) const {
    if (empty()) return {b, nullptr};
    if (size() != IPv4len && size() != IPv6len) {
        return {b, make_ptr<AddrError>(string::fromStatic("invalid IP address"), string(hexString(span())))};
    }
    std::string s;
    appendTo(s, *this);
    return {appendSpan(std::move(b), as_bytes(std::string_view(s))), nullptr};
}

std::pair<slice<byte>, error> IP::MarshalText() const {
    auto [b, err] = AppendText(slice<byte>::make(0, 24));
    if (err) return {{}, err};
    return {b, nullptr};
}

error IP::UnmarshalText(std::span<const byte> text) {
    if (text.empty()) {
        *this = IP();
        return nullptr;
    }
    std::string_view s(reinterpret_cast<const char*>(text.data()), text.size());
    IP x = ParseIP(s);
    if (x.empty()) return make_ptr<ParseError>(string::fromStatic("IP address"), string(s));
    *this = x;
    return nullptr;
}

IP ParseIP(std::string_view s) {
    auto ip = parseIP(s);
    if (!ip) return {};
    return IP(slice<byte>::of(*ip));
}

std::pair<int, int> IPMask::Size() const {
    int ones = simpleMaskLength(span()), bits = static_cast<int>(size()) * 8;
    if (ones == -1) return {0, 0};
    return {ones, bits};
}

string IPMask::String() const {
    if (empty()) return string::fromStatic("<nil>");
    return string(hexString(span()));
}

bool IPNet::Contains(const net::IP& ip0) const {
    auto [nn, m] = networkNumberAndMask(*this);
    net::IP ip = ip0;
    if (net::IP x = ip.To4(); !x.empty()) ip = x;
    std::size_t l = ip.size();
    if (l != nn.size()) return false;
    for (std::size_t i = 0; i < l; i++) {
        if ((nn[i] & m[i]) != (ip[i] & m[i])) return false;
    }
    return true;
}

string IPNet::String() const {
    auto [nn, m] = networkNumberAndMask(*this);
    if (nn.empty() || m.empty()) return string::fromStatic("<nil>");
    int l = simpleMaskLength(m.span());
    if (l == -1) return nn.String() + "/" + m.String();
    return nn.String() + "/" + string(std::to_string(l));
}

std::tuple<IP, ptr<IPNet>, error> ParseCIDR(std::string_view s) {
    auto bad = [&] { return std::tuple<IP, ptr<IPNet>, error>{{}, nullptr, make_ptr<ParseError>(string::fromStatic("CIDR address"), string(s))}; };
    auto slash = s.find('/');
    if (slash == s.npos) return bad();
    std::string_view addr = s.substr(0, slash), mask = s.substr(slash + 1);
    auto ip = parseIP(addr);
    if (!ip) return bad();
    int bits = addr.find(':') == addr.npos ? 32 : 128;
    // Go's dtoi: decimal digits, at most 0xFFFFFF.
    if (mask.empty()) return bad();
    int n = 0;
    for (char c : mask) {
        if (c < '0' || c > '9') return bad();
        n = n * 10 + (c - '0');
        if (n >= 0xFFFFFF) return bad();
    }
    if (n > bits) return bad();
    IPMask m = CIDRMask(n, bits);
    IP ip16(slice<byte>::of(*ip));
    return {ip16, make_ptr<IPNet>(ip16.Mask(m.span()), m), nullptr};
}

}  // namespace gopp::net
