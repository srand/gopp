// Ported from Go 1.27.3's net/netip/netip.go and uint128.go.
#include <gopp/net/netip.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gopp::netip {

uint128 mask6(int n) noexcept {
    // Go's shifts by 64 or more give 0; C++'s are undefined.
    uint64 hi = n >= 64 ? ~uint64(0) : n <= 0 ? 0 : ~(~uint64(0) >> n);
    uint64 lo = n <= 64 ? 0 : n >= 128 ? ~uint64(0) : ~uint64(0) << (128 - n);
    return {hi, lo};
}

uint128 uint128::bitsSetFrom(uint8 bit) const noexcept { return or_(mask6(bit).not_()); }
uint128 uint128::bitsClearedFrom(uint8 bit) const noexcept { return and_(mask6(bit)); }

namespace {

const detail::addrDetail z4Detail{false, {}};
const detail::addrDetail z6nozDetail{true, {}};

// The zones seen, interned (Go's unique.Make). They live as long as the
// program, as unique's handles do while used.
struct zoneTable {
    std::mutex mu;
    std::vector<std::unique_ptr<detail::addrDetail>> details;
    std::unordered_map<std::string_view, const detail::addrDetail*> byzone;
};

zoneTable& zones() {
    static auto* t = new zoneTable;
    return *t;
}

std::string_view sv(const string& s) { return s.view(); }

// Go's parseAddrError.
struct parseAddrError final : errorInterface {
    string in, msg, at;
    parseAddrError(std::string_view in_, std::string_view msg_, std::string_view at_ = {})
        : in(in_), msg(msg_), at(at_) {}
    string Error() const override {
        string s = "ParseAddr(" + strconv::Quote(in.view()) + "): " + msg;
        if (!at.empty()) s = s + " (at " + strconv::Quote(at.view()) + ")";
        return s;
    }
};

error addrErr(std::string_view in, std::string_view msg, std::string_view at = {}) {
    return make_ptr<parseAddrError>(in, msg, at);
}

// Go's parsePrefixError.
struct parsePrefixError final : errorInterface {
    string in, msg;
    parsePrefixError(std::string_view in_, string msg_) : in(in_), msg(std::move(msg_)) {}
    string Error() const override { return "netip.ParsePrefix(" + strconv::Quote(in.view()) + "): " + msg; }
};

error prefixErr(std::string_view in, string msg) { return make_ptr<parsePrefixError>(in, std::move(msg)); }

uint64 be64(const byte* p) {
    uint64 v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

void putbe64(byte* p, uint64 v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = byte(v);
        v >>= 8;
    }
}

slice<byte> appendBytes(slice<byte> b, std::string_view s) { return appendSpan(std::move(b), as_bytes(s)); }

slice<byte> appendByte(slice<byte> b, char c) { return append(std::move(b), byte(c)); }

constexpr std::string_view digits = "0123456789abcdef";

slice<byte> appendDecimal(slice<byte> b, uint8 x) {
    if (x >= 100) b = appendByte(std::move(b), digits[x / 100]);
    if (x >= 10) b = appendByte(std::move(b), digits[x / 10 % 10]);
    return appendByte(std::move(b), digits[x % 10]);
}

slice<byte> appendHex(slice<byte> b, uint16 x) {
    if (x >= 0x1000) b = appendByte(std::move(b), digits[x >> 12]);
    if (x >= 0x100) b = appendByte(std::move(b), digits[x >> 8 & 0xf]);
    if (x >= 0x10) b = appendByte(std::move(b), digits[x >> 4 & 0xf]);
    return appendByte(std::move(b), digits[x & 0xf]);
}

slice<byte> appendHexPad(slice<byte> b, uint16 x) {
    return append(std::move(b), byte(digits[x >> 12]), byte(digits[x >> 8 & 0xf]), byte(digits[x >> 4 & 0xf]),
                  byte(digits[x & 0xf]));
}

slice<byte> appendUint(slice<byte> b, uint64 v) { return appendBytes(std::move(b), std::to_string(v)); }

string bytesString(const slice<byte>& b) { return String(b); }

// Go's parseIPv4Fields: the four fields of in[off:end] into fields.
error parseIPv4Fields(std::string_view in, std::size_t off, std::size_t end, byte* fields) {
    int val = 0, pos = 0, digLen = 0;
    std::string_view s = in.substr(off, end - off);
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (digLen == 1 && val == 0) return addrErr(in, "IPv4 field has octet with leading zero");
            val = val * 10 + (s[i] - '0');
            digLen++;
            if (val > 255) return addrErr(in, "IPv4 field has value >255");
        } else if (s[i] == '.') {
            // .1.2.3, 1.2.3. and 1..2.3 are invalid.
            if (i == 0 || i == s.size() - 1 || s[i - 1] == '.') {
                return addrErr(in, "IPv4 field must have at least one digit", s.substr(i));
            }
            if (pos == 3) return addrErr(in, "IPv4 address too long");
            fields[pos++] = byte(val);
            val = 0;
            digLen = 0;
        } else {
            return addrErr(in, "unexpected character", s.substr(i));
        }
    }
    if (pos < 3) return addrErr(in, "IPv4 address too short");
    fields[3] = byte(val);
    return nullptr;
}

std::pair<Addr, error> parseIPv4(std::string_view s) {
    std::array<byte, 4> fields{};
    if (error err = parseIPv4Fields(s, 0, s.size(), fields.data())) return {Addr{}, err};
    return {AddrFrom4(fields), nullptr};
}

std::pair<Addr, error> parseIPv6(std::string_view in) {
    std::string_view s = in, zone;
    if (auto i = s.find('%'); i != s.npos) {
        zone = s.substr(i + 1);
        s = s.substr(0, i);
        if (zone.empty()) return {Addr{}, addrErr(in, "zone must be a non-empty string")};
    }
    std::array<byte, 16> ip{};
    int ellipsis = -1;  // position of the ellipsis in ip
    // Might have a leading ellipsis.
    if (s.size() >= 2 && s[0] == ':' && s[1] == ':') {
        ellipsis = 0;
        s.remove_prefix(2);
        if (s.empty()) return {IPv6Unspecified().WithZone(zone), nullptr};
    }
    int i = 0;
    while (i < 16) {
        // A hex number.
        std::size_t off = 0;
        uint32 acc = 0;
        for (; off < s.size(); off++) {
            char c = s[off];
            if (c >= '0' && c <= '9') acc = (acc << 4) + uint32(c - '0');
            else if (c >= 'a' && c <= 'f') acc = (acc << 4) + uint32(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') acc = (acc << 4) + uint32(c - 'A' + 10);
            else break;
            if (off > 3) return {Addr{}, addrErr(in, "each group must have 4 or less digits", s)};
            if (acc > 0xffff) return {Addr{}, addrErr(in, "IPv6 field has value >=2^16", s)};
        }
        if (off == 0) return {Addr{}, addrErr(in, "each colon-separated field must have at least one digit", s)};
        // An IPv4 address in the last 32 bits?
        if (off < s.size() && s[off] == '.') {
            if (ellipsis < 0 && i != 12) {
                return {Addr{}, addrErr(in, "embedded IPv4 address must replace the final 2 fields of the address", s)};
            }
            if (i + 4 > 16) {
                return {Addr{}, addrErr(in, "too many hex fields to fit an embedded IPv4 at the end of the address", s)};
            }
            std::size_t end = in.size();
            if (!zone.empty()) end -= zone.size() + 1;
            if (error err = parseIPv4Fields(in, end - s.size(), end, ip.data() + i)) return {Addr{}, err};
            s = {};
            i += 4;
            break;
        }
        ip[i] = byte(acc >> 8);
        ip[i + 1] = byte(acc);
        i += 2;
        s.remove_prefix(off);
        if (s.empty()) break;
        // Otherwise a colon must follow.
        if (s[0] != ':') return {Addr{}, addrErr(in, "unexpected character, want colon", s)};
        if (s.size() == 1) return {Addr{}, addrErr(in, "colon must be followed by more characters", s)};
        s.remove_prefix(1);
        // An ellipsis?
        if (s[0] == ':') {
            if (ellipsis >= 0) return {Addr{}, addrErr(in, "multiple :: in address", s)};
            ellipsis = i;
            s.remove_prefix(1);
            if (s.empty()) break;  // can be at end
        }
    }
    if (!s.empty()) return {Addr{}, addrErr(in, "trailing garbage after address", s)};
    if (i < 16) {
        if (ellipsis < 0) return {Addr{}, addrErr(in, "address string too short")};
        int n = 16 - i;
        for (int j = i - 1; j >= ellipsis; j--) ip[j + n] = ip[j];
        for (int j = ellipsis; j < ellipsis + n; j++) ip[j] = 0;
    } else if (ellipsis >= 0) {
        // The ellipsis must stand for at least one field of zeros.
        return {Addr{}, addrErr(in, "the :: must expand to at least one field of zeros")};
    }
    return {AddrFrom16(ip).WithZone(zone), nullptr};
}

std::pair<std::string_view, std::string_view> lastSplit(std::string_view s, char c, bool* ok) {
    auto i = s.rfind(c);
    *ok = i != s.npos;
    if (!*ok) return {};
    return {s.substr(0, i), s.substr(i + 1)};
}

// strconv.ParseUint(s, 10, bits) for the ports and prefix lengths here.
bool parseUint(std::string_view s, uint64 max, uint64* v) {
    if (s.empty()) return false;
    uint64 n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + uint64(c - '0');
        if (n > max) return false;
    }
    *v = n;
    return true;
}

// strconv.Atoi, for ParsePrefix: an optional sign, then digits.
bool atoi(std::string_view s, int* v) {
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        s.remove_prefix(1);
    }
    uint64 n;
    if (!parseUint(s, uint64(1) << 31, &n)) return false;
    *v = neg ? -int(n) : int(n);
    return true;
}

}  // namespace

const detail::addrDetail* const z4 = &z4Detail;
const detail::addrDetail* const z6noz = &z6nozDetail;

const detail::addrDetail* MakeAddrDetail(bool isV6, std::string_view zone) {
    if (!isV6) return z4;
    if (zone.empty()) return z6noz;
    auto& t = zones();
    std::lock_guard lock(t.mu);
    if (auto it = t.byzone.find(zone); it != t.byzone.end()) return it->second;
    auto d = std::make_unique<detail::addrDetail>(detail::addrDetail{true, string(zone)});
    const detail::addrDetail* p = d.get();
    t.byzone.emplace(sv(p->zoneV6), p);
    t.details.push_back(std::move(d));
    return p;
}

Addr IPv6LinkLocalAllNodes() noexcept { return AddrFrom16({0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}); }
Addr IPv6LinkLocalAllRouters() noexcept { return AddrFrom16({0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02}); }
Addr IPv6Loopback() noexcept { return AddrFrom16({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}); }
Addr IPv6Unspecified() noexcept { return Addr(uint128{}, z6noz); }
Addr IPv4Unspecified() noexcept { return AddrFrom4({}); }

Addr AddrFrom4(std::array<byte, 4> a) noexcept {
    return Addr(uint128{0, 0xffff00000000 | uint64(a[0]) << 24 | uint64(a[1]) << 16 | uint64(a[2]) << 8 | uint64(a[3])},
                z4);
}

Addr AddrFrom16(std::array<byte, 16> a) noexcept { return Addr(uint128{be64(a.data()), be64(a.data() + 8)}, z6noz); }

std::pair<Addr, bool> AddrFromSlice(std::span<const byte> s) noexcept {
    if (s.size() == 4) return {AddrFrom4({s[0], s[1], s[2], s[3]}), true};
    if (s.size() == 16) {
        std::array<byte, 16> a;
        std::copy(s.begin(), s.end(), a.begin());
        return {AddrFrom16(a), true};
    }
    return {Addr{}, false};
}

std::pair<Addr, error> ParseAddr(std::string_view s) {
    for (char c : s) {
        switch (c) {
        case '.': return parseIPv4(s);
        case ':': return parseIPv6(s);
        case '%':
            // Assume that this was trying to be an IPv6 address with a zone
            // specifier, but the address is missing.
            return {Addr{}, addrErr(s, "missing IPv6 address")};
        }
    }
    return {Addr{}, addrErr(s, "unable to parse IP")};
}

Addr MustParseAddr(std::string_view s) {
    auto [ip, err] = ParseAddr(s);
    if (err) panic(err.Error().view());
    return ip;
}

int Addr::BitLen() const noexcept {
    if (z_ == nullptr) return 0;
    if (z_ == z4) return 32;
    return 128;
}

string Addr::Zone() const {
    if (z_ == nullptr) return {};
    return z_->zoneV6;
}

int Addr::Compare(const Addr& ip2) const {
    int f1 = BitLen(), f2 = ip2.BitLen();
    if (f1 != f2) return f1 < f2 ? -1 : 1;
    if (addr_.hi != ip2.addr_.hi) return addr_.hi < ip2.addr_.hi ? -1 : 1;
    if (addr_.lo != ip2.addr_.lo) return addr_.lo < ip2.addr_.lo ? -1 : 1;
    if (Is6()) {
        auto c = Zone().view().compare(ip2.Zone().view());
        if (c != 0) return c < 0 ? -1 : 1;
    }
    return 0;
}

bool Addr::Is4() const noexcept { return z_ == z4; }
bool Addr::Is4In6() const noexcept { return Is6() && addr_.hi == 0 && addr_.lo >> 32 == 0xffff; }
bool Addr::Is6() const noexcept { return z_ != nullptr && z_ != z4; }

Addr Addr::Unmap() const noexcept {
    Addr ip = *this;
    if (ip.Is4In6()) ip.z_ = z4;
    return ip;
}

Addr Addr::WithZone(std::string_view zone) const {
    if (!Is6()) return *this;
    Addr ip = *this;
    ip.z_ = MakeAddrDetail(true, zone);
    return ip;
}

Addr Addr::withoutZone() const noexcept {
    if (!Is6()) return *this;
    Addr ip = *this;
    ip.z_ = z6noz;
    return ip;
}

bool Addr::hasZone() const noexcept { return z_ != nullptr && z_ != z4 && z_ != z6noz; }

bool Addr::IsLinkLocalUnicast() const noexcept {
    Addr ip = Is4In6() ? Unmap() : *this;
    // RFC 3927 (169.254.0.0/16) and RFC 4291 (fe80::/10).
    if (ip.Is4()) return ip.v4(0) == 169 && ip.v4(1) == 254;
    if (ip.Is6()) return (ip.v6u16(0) & 0xffc0) == 0xfe80;
    return false;
}

bool Addr::IsLoopback() const noexcept {
    Addr ip = Is4In6() ? Unmap() : *this;
    if (ip.Is4()) return ip.v4(0) == 127;
    if (ip.Is6()) return ip.addr_.hi == 0 && ip.addr_.lo == 1;
    return false;
}

bool Addr::IsMulticast() const noexcept {
    Addr ip = Is4In6() ? Unmap() : *this;
    if (ip.Is4()) return (ip.v4(0) & 0xf0) == 0xe0;
    if (ip.Is6()) return ip.addr_.hi >> 56 == 0xff;
    return false;
}

bool Addr::IsInterfaceLocalMulticast() const noexcept {
    // IPv4 has no interface-local scope.
    if (Is6() && !Is4In6()) return (v6u16(0) & 0xff0f) == 0xff01;
    return false;
}

bool Addr::IsLinkLocalMulticast() const noexcept {
    Addr ip = Is4In6() ? Unmap() : *this;
    if (ip.Is4()) return ip.v4(0) == 224 && ip.v4(1) == 0 && ip.v4(2) == 0;
    if (ip.Is6()) return (ip.v6u16(0) & 0xff0f) == 0xff02;
    return false;
}

bool Addr::IsGlobalUnicast() const noexcept {
    if (z_ == nullptr) return false;
    Addr ip = Is4In6() ? Unmap() : *this;
    // The IPv4 unspecified and limited broadcast addresses aren't.
    if (ip.Is4() && (ip == IPv4Unspecified() || ip == AddrFrom4({255, 255, 255, 255}))) return false;
    return ip != IPv6Unspecified() && !ip.IsLoopback() && !ip.IsMulticast() && !ip.IsLinkLocalUnicast();
}

bool Addr::IsPrivate() const noexcept {
    Addr ip = Is4In6() ? Unmap() : *this;
    // RFC 1918 and RFC 4193.
    if (ip.Is4()) {
        return ip.v4(0) == 10 || (ip.v4(0) == 172 && (ip.v4(1) & 0xf0) == 16) ||
               (ip.v4(0) == 192 && ip.v4(1) == 168);
    }
    if (ip.Is6()) return (ip.v6(0) & 0xfe) == 0xfc;
    return false;
}

bool Addr::IsUnspecified() const noexcept { return *this == IPv4Unspecified() || *this == IPv6Unspecified(); }

std::pair<netip::Prefix, error> Addr::Prefix(int b) const {
    if (b < 0) return {netip::Prefix{}, errors::New("negative Prefix bits")};
    int effectiveBits = b;
    if (z_ == nullptr) return {netip::Prefix{}, nullptr};
    if (z_ == z4) {
        if (b > 32) return {netip::Prefix{}, errors::New("prefix length " + std::to_string(b) + " too large for IPv4")};
        effectiveBits += 96;
    } else if (b > 128) {
        return {netip::Prefix{}, errors::New("prefix length " + std::to_string(b) + " too large for IPv6")};
    }
    Addr ip = *this;
    ip.addr_ = ip.addr_.and_(mask6(effectiveBits));
    return {PrefixFrom(ip, b), nullptr};
}

std::array<byte, 16> Addr::As16() const noexcept {
    std::array<byte, 16> a;
    putbe64(a.data(), addr_.hi);
    putbe64(a.data() + 8, addr_.lo);
    return a;
}

std::array<byte, 4> Addr::As4() const {
    if (z_ == z4 || Is4In6()) {
        uint32 v = uint32(addr_.lo);
        return {byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)};
    }
    if (z_ == nullptr) panic("As4 called on IP zero value");
    panic("As4 called on IPv6 address");
}

slice<byte> Addr::AsSlice() const {
    if (z_ == nullptr) return {};
    if (z_ == z4) {
        auto a = As4();
        return slice<byte>::of(a);
    }
    auto a = As16();
    return slice<byte>::of(a);
}

Addr Addr::Next() const noexcept {
    Addr ip = *this;
    ip.addr_ = ip.addr_.addOne();
    if (ip.Is4()) {
        if (uint32(ip.addr_.lo) == 0) return Addr{};  // overflowed
    } else if (ip.addr_.isZero()) {
        return Addr{};
    }
    return ip;
}

Addr Addr::Prev() const noexcept {
    if (Is4()) {
        if (uint32(addr_.lo) == 0) return Addr{};
    } else if (addr_.isZero()) {
        return Addr{};
    }
    Addr ip = *this;
    ip.addr_ = ip.addr_.subOne();
    return ip;
}

slice<byte> Addr::appendTo4(slice<byte> b) const {
    b = appendDecimal(std::move(b), v4(0));
    b = appendByte(std::move(b), '.');
    b = appendDecimal(std::move(b), v4(1));
    b = appendByte(std::move(b), '.');
    b = appendDecimal(std::move(b), v4(2));
    b = appendByte(std::move(b), '.');
    return appendDecimal(std::move(b), v4(3));
}

slice<byte> Addr::appendTo4In6(slice<byte> b) const {
    b = appendBytes(std::move(b), "::ffff:");
    b = Unmap().appendTo4(std::move(b));
    if (z_ != z6noz) {
        b = appendByte(std::move(b), '%');
        b = appendBytes(std::move(b), sv(z_->zoneV6));
    }
    return b;
}

// RFC 5952: the longest run of two or more zero fields, the first of
// equal ones, is elided.
slice<byte> Addr::appendTo6(slice<byte> b) const {
    uint8 zeroStart = 255, zeroEnd = 255;
    for (uint8 i = 0; i < 8; i++) {
        uint8 j = i;
        while (j < 8 && v6u16(j) == 0) j++;
        if (uint8 l = j - i; l >= 2 && l > uint8(zeroEnd - zeroStart)) {
            zeroStart = i;
            zeroEnd = j;
        }
    }
    for (uint8 i = 0; i < 8; i++) {
        if (i == zeroStart) {
            b = append(std::move(b), byte(':'), byte(':'));
            i = zeroEnd;
            if (i >= 8) break;
        } else if (i > 0) {
            b = appendByte(std::move(b), ':');
        }
        b = appendHex(std::move(b), v6u16(i));
    }
    if (z_ != z6noz) {
        b = appendByte(std::move(b), '%');
        b = appendBytes(std::move(b), sv(z_->zoneV6));
    }
    return b;
}

string Addr::String() const {
    if (!IsValid()) return string::fromStatic("invalid IP");
    slice<byte> b = slice<byte>::make(0, 46);
    if (z_ == z4) b = appendTo4(std::move(b));
    else if (Is4In6()) b = appendTo4In6(std::move(b));
    else b = appendTo6(std::move(b));
    return bytesString(b);
}

slice<byte> Addr::AppendTo(slice<byte> b) const {
    if (z_ == nullptr) return b;
    if (z_ == z4) return appendTo4(std::move(b));
    if (Is4In6()) return appendTo4In6(std::move(b));
    return appendTo6(std::move(b));
}

string Addr::StringExpanded() const {
    if (z_ == nullptr || z_ == z4) return String();
    slice<byte> b = slice<byte>::make(0, 39);
    for (uint8 i = 0; i < 8; i++) {
        if (i > 0) b = appendByte(std::move(b), ':');
        b = appendHexPad(std::move(b), v6u16(i));
    }
    if (z_ != z6noz) {
        b = appendByte(std::move(b), '%');
        b = appendBytes(std::move(b), sv(z_->zoneV6));
    }
    return bytesString(b);
}

std::pair<slice<byte>, error> Addr::MarshalText() const {
    // Empty, not nil, for the zero Addr.
    return AppendText(slice<byte>::make(0, z_ == nullptr ? 0 : 46));
}

error Addr::UnmarshalText(std::span<const byte> text) {
    if (text.empty()) {
        *this = Addr{};
        return nullptr;
    }
    error err;
    std::tie(*this, err) = ParseAddr(asStringView(text));
    return err;
}

std::pair<slice<byte>, error> Addr::AppendBinary(slice<byte> b) const {
    if (z_ == nullptr) return {b, nullptr};
    byte buf[16];
    if (z_ == z4) {
        uint32 v = uint32(addr_.lo);
        byte four[4] = {byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)};
        return {appendSpan(std::move(b), std::span<const byte>(four)), nullptr};
    }
    putbe64(buf, addr_.hi);
    putbe64(buf + 8, addr_.lo);
    b = appendSpan(std::move(b), std::span<const byte>(buf));
    return {appendBytes(std::move(b), sv(Zone())), nullptr};
}

int Addr::marshalBinarySize() const {
    if (z_ == nullptr) return 0;
    if (z_ == z4) return 4;
    return 16 + int(Zone().size());
}

std::pair<slice<byte>, error> Addr::MarshalBinary() const {
    return AppendBinary(slice<byte>::make(0, std::size_t(marshalBinarySize())));
}

error Addr::UnmarshalBinary(std::span<const byte> b) {
    std::size_t n = b.size();
    if (n == 0) {
        *this = Addr{};
        return nullptr;
    }
    if (n == 4) {
        *this = AddrFrom4({b[0], b[1], b[2], b[3]});
        return nullptr;
    }
    if (n >= 16) {
        std::array<byte, 16> a;
        std::copy_n(b.begin(), 16, a.begin());
        *this = AddrFrom16(a).WithZone(asStringView(b.subspan(16)));
        return nullptr;
    }
    return errors::New("unexpected slice size");
}

// ---------------------------------------------------------------------------
// AddrPort

AddrPort AddrPortFrom(Addr ip, uint16 port) noexcept { return AddrPort(ip, port); }

namespace {

// Go's splitAddrPort: s as an IP and a port, without parsing either.
error splitAddrPort(std::string_view s, std::string_view* ip, std::string_view* port, bool* v6) {
    bool ok;
    std::tie(*ip, *port) = lastSplit(s, ':', &ok);
    *v6 = false;
    if (!ok) return errors::New("not an ip:port");
    if (ip->empty()) return errors::New("no IP");
    if (port->empty()) return errors::New("no port");
    if ((*ip)[0] == '[') {
        if (ip->size() < 2 || ip->back() != ']') return errors::New("missing ]");
        *ip = ip->substr(1, ip->size() - 2);
        *v6 = true;
    }
    return nullptr;
}

}  // namespace

std::pair<AddrPort, error> ParseAddrPort(std::string_view s) {
    std::string_view ip, port;
    bool v6;
    if (error err = splitAddrPort(s, &ip, &port, &v6)) return {AddrPort{}, err};
    uint64 port16;
    if (!parseUint(port, 0xffff, &port16)) {
        return {AddrPort{}, errors::New("invalid port " + strconv::Quote(port) + " parsing " + strconv::Quote(s))};
    }
    auto [addr, err] = ParseAddr(ip);
    if (err) return {AddrPort{}, err};
    if (v6 && addr.Is4()) {
        return {AddrPort{}, errors::New("invalid ip:port " + strconv::Quote(s) +
                                        ", square brackets can only be used with IPv6 addresses")};
    }
    if (!v6 && addr.Is6()) {
        return {AddrPort{}, errors::New("invalid ip:port " + strconv::Quote(s) +
                                        ", IPv6 addresses must be surrounded by square brackets")};
    }
    return {AddrPort(addr, uint16(port16)), nullptr};
}

AddrPort MustParseAddrPort(std::string_view s) {
    auto [ip, err] = ParseAddrPort(s);
    if (err) panic(err.Error().view());
    return ip;
}

int AddrPort::Compare(const AddrPort& p2) const {
    if (int c = ip_.Compare(p2.ip_); c != 0) return c;
    return port_ < p2.port_ ? -1 : port_ > p2.port_ ? 1 : 0;
}

string AddrPort::String() const {
    if (ip_.z() == nullptr) return string::fromStatic("invalid AddrPort");
    return bytesString(AppendTo(slice<byte>::make(0, 54)));
}

slice<byte> AddrPort::AppendTo(slice<byte> b) const {
    if (ip_.z() == nullptr) return b;
    if (ip_.z() == z4) {
        b = ip_.appendTo4(std::move(b));
    } else {
        b = appendByte(std::move(b), '[');
        b = ip_.Is4In6() ? ip_.appendTo4In6(std::move(b)) : ip_.appendTo6(std::move(b));
        b = appendByte(std::move(b), ']');
    }
    b = appendByte(std::move(b), ':');
    return appendUint(std::move(b), port_);
}

std::pair<slice<byte>, error> AddrPort::MarshalText() const {
    return AppendText(slice<byte>::make(0, ip_.z() == nullptr ? 0 : 54));
}

error AddrPort::UnmarshalText(std::span<const byte> text) {
    if (text.empty()) {
        *this = AddrPort{};
        return nullptr;
    }
    error err;
    std::tie(*this, err) = ParseAddrPort(asStringView(text));
    return err;
}

std::pair<slice<byte>, error> AddrPort::AppendBinary(slice<byte> b) const {
    auto [r, err] = ip_.AppendBinary(std::move(b));
    if (err) return {{}, err};
    return {append(std::move(r), byte(port_), byte(port_ >> 8)), nullptr};
}

std::pair<slice<byte>, error> AddrPort::MarshalBinary() const {
    return AppendBinary(slice<byte>::make(0, std::size_t(ip_.marshalBinarySize() + 2)));
}

error AddrPort::UnmarshalBinary(std::span<const byte> b) {
    if (b.size() < 2) return errors::New("unexpected slice size");
    netip::Addr addr;
    if (error err = addr.UnmarshalBinary(b.first(b.size() - 2))) return err;
    *this = AddrPort(addr, uint16(b[b.size() - 2] | b[b.size() - 1] << 8));
    return nullptr;
}

// ---------------------------------------------------------------------------
// Prefix

Prefix PrefixFrom(Addr ip, int bits) {
    uint8 bitsPlusOne = 0;
    if (!ip.isZero() && bits >= 0 && bits <= ip.BitLen()) bitsPlusOne = uint8(bits + 1);
    return Prefix(ip.withoutZone(), bitsPlusOne);
}

int Prefix::Compare(const Prefix& p2) const {
    // Aside from sorting by family, IANA's registries list prefixes by
    // their masked address, then by length, then by address.
    if (int c = Masked().Addr().Compare(p2.Masked().Addr()); c != 0) return c;
    if (Bits() != p2.Bits()) return Bits() < p2.Bits() ? -1 : 1;
    return ip_.Compare(p2.ip_);
}

std::pair<Prefix, error> ParsePrefix(std::string_view s) {
    auto i = s.rfind('/');
    if (i == s.npos) return {Prefix{}, prefixErr(s, string::fromStatic("no '/'"))};
    auto [ip, err] = ParseAddr(s.substr(0, i));
    if (err) return {Prefix{}, prefixErr(s, err.Error())};
    // Zones aren't allowed in prefixes (go.dev/issue/51899).
    if (ip.Is6() && ip.z() != z6noz) {
        return {Prefix{}, prefixErr(s, string::fromStatic("IPv6 zones cannot be present in a prefix"))};
    }
    std::string_view bitsStr = s.substr(i + 1);
    // strconv.Atoi accepts a sign and leading zeros; prefixes don't.
    if (bitsStr.size() > 1 && (bitsStr[0] < '1' || bitsStr[0] > '9')) {
        return {Prefix{}, prefixErr(s, "bad bits after slash: " + strconv::Quote(bitsStr))};
    }
    int bits;
    if (!atoi(bitsStr, &bits)) return {Prefix{}, prefixErr(s, "bad bits after slash: " + strconv::Quote(bitsStr))};
    int maxBits = ip.Is6() ? 128 : 32;
    if (bits < 0 || bits > maxBits) return {Prefix{}, prefixErr(s, string::fromStatic("prefix length out of range"))};
    return {PrefixFrom(ip, bits), nullptr};
}

Prefix MustParsePrefix(std::string_view s) {
    auto [p, err] = ParsePrefix(s);
    if (err) panic(err.Error().view());
    return p;
}

Prefix Prefix::Masked() const { return ip_.Prefix(Bits()).first; }

bool Prefix::Contains(const netip::Addr& ip) const noexcept {
    if (!IsValid() || ip.hasZone()) return false;
    int f1 = ip_.BitLen(), f2 = ip.BitLen();
    if (f1 == 0 || f2 == 0 || f1 != f2) return false;
    if (ip.Is4()) {
        // The IPv4 bits are the low 32 of lo; shifting the XOR right by the
        // bits outside the prefix leaves the prefix's differences.
        return uint32((ip.addr_.lo ^ ip_.addr_.lo) >> ((32 - Bits()) & 63)) == 0;
    }
    return ip.addr_.xor_(ip_.addr_).and_(mask6(Bits())).isZero();
}

bool Prefix::Overlaps(const Prefix& o) const {
    if (!IsValid() || !o.IsValid()) return false;
    if (*this == o) return true;
    if (ip_.Is4() != o.ip_.Is4()) return false;
    int minBits = std::min(Bits(), o.Bits());
    if (minBits == 0) return true;
    auto [p, err] = ip_.Prefix(minBits);
    if (err) return false;
    auto [q, err2] = o.ip_.Prefix(minBits);
    if (err2) return false;
    return p.ip_ == q.ip_;
}

slice<byte> Prefix::AppendTo(slice<byte> b) const {
    if (isZero()) return b;
    if (!IsValid()) return appendBytes(std::move(b), "invalid Prefix");
    if (ip_.z() == z4) {
        b = ip_.appendTo4(std::move(b));
    } else if (ip_.Is4In6()) {
        b = appendBytes(std::move(b), "::ffff:");
        b = ip_.Unmap().appendTo4(std::move(b));
    } else {
        b = ip_.appendTo6(std::move(b));
    }
    b = appendByte(std::move(b), '/');
    return appendDecimal(std::move(b), uint8(Bits()));
}

std::pair<slice<byte>, error> Prefix::MarshalText() const {
    return AppendText(slice<byte>::make(0, ip_.z() == nullptr ? 0 : 50));
}

error Prefix::UnmarshalText(std::span<const byte> text) {
    if (text.empty()) {
        *this = Prefix{};
        return nullptr;
    }
    error err;
    std::tie(*this, err) = ParsePrefix(asStringView(text));
    return err;
}

std::pair<slice<byte>, error> Prefix::AppendBinary(slice<byte> b) const {
    auto [r, err] = ip_.withoutZone().AppendBinary(std::move(b));
    if (err) return {{}, err};
    return {append(std::move(r), byte(Bits())), nullptr};
}

std::pair<slice<byte>, error> Prefix::MarshalBinary() const {
    return AppendBinary(slice<byte>::make(0, std::size_t(ip_.withoutZone().marshalBinarySize() + 1)));
}

error Prefix::UnmarshalBinary(std::span<const byte> b) {
    if (b.empty()) return errors::New("unexpected slice size");
    netip::Addr addr;
    if (error err = addr.UnmarshalBinary(b.first(b.size() - 1))) return err;
    *this = PrefixFrom(addr, int(b.back()));
    return nullptr;
}

string Prefix::String() const {
    if (!IsValid()) return string::fromStatic("invalid Prefix");
    return bytesString(AppendTo(slice<byte>::make(0, 43)));
}

}  // namespace gopp::netip
