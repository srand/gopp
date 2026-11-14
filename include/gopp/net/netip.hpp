// The net/netip package: IP addresses as small values.
//
// netip::Addr is an IPv4 or IPv6 address (with an optional zone), and
// unlike net::IP it's a comparable value: == works, it's cheap to copy, and
// it can be a map key. AddrPort adds a port and Prefix a prefix length.
//
//     auto [ip, err] = netip::ParseAddr("2001:db8::1");
//     auto p = netip::MustParsePrefix("2001:db8::/32");
//     if (p.Contains(ip)) ...
//     auto ap = netip::AddrPortFrom(ip, 443);   // [2001:db8::1]:443
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#endif

namespace gopp::netip {

// A 128-bit number: an address's bits, hi first (Go's uint128). Exposed for
// the tests, as Go's export_test.go does.
struct uint128 {
    uint64 hi = 0;
    uint64 lo = 0;

    constexpr bool isZero() const noexcept { return (hi | lo) == 0; }
    constexpr uint128 and_(uint128 m) const noexcept { return {hi & m.hi, lo & m.lo}; }
    constexpr uint128 xor_(uint128 m) const noexcept { return {hi ^ m.hi, lo ^ m.lo}; }
    constexpr uint128 or_(uint128 m) const noexcept { return {hi | m.hi, lo | m.lo}; }
    constexpr uint128 not_() const noexcept { return {~hi, ~lo}; }
    constexpr uint128 subOne() const noexcept { return {hi - (lo == 0 ? 1 : 0), lo - 1}; }
    constexpr uint128 addOne() const noexcept { return {hi + (lo == ~uint64(0) ? 1 : 0), lo + 1}; }
    uint128 bitsSetFrom(uint8 bit) const noexcept;
    uint128 bitsClearedFrom(uint8 bit) const noexcept;
    friend constexpr bool operator==(uint128, uint128) noexcept = default;
};

// mask6(n): the first n bits set.
uint128 mask6(int n) noexcept;

namespace detail {
// What Go's unique.Handle[addrDetail] points at: an address's family and
// zone, interned, so that Addrs compare by pointer.
struct addrDetail {
    bool isV6;
    string zoneV6;
};
}  // namespace detail

class Prefix;

// netip.Addr: an IPv4 or IPv6 address, or the zero Addr, which is none.
class Addr {
public:
    constexpr Addr() noexcept = default;

    // IsValid: whether the address isn't the zero Addr.
    bool IsValid() const noexcept { return z_ != nullptr; }
    // BitLen: 32 for IPv4, 128 for IPv6, 0 for the zero Addr.
    int BitLen() const noexcept;
    // Zone: the IPv6 scoped addressing zone, if any.
    string Zone() const;
    // Compare: -1, 0 or +1; IPv4 before IPv6, then by bits, then by zone.
    int Compare(const Addr& ip2) const;
    bool Less(const Addr& ip2) const { return Compare(ip2) == -1; }
    bool Is4() const noexcept;
    // Is4In6: an IPv4-mapped IPv6 address (::ffff:a.b.c.d).
    bool Is4In6() const noexcept;
    bool Is6() const noexcept;
    // Unmap: an IPv4-mapped IPv6 address as IPv4; others unchanged.
    Addr Unmap() const noexcept;
    // WithZone: the address with zone; IPv4 addresses have none.
    Addr WithZone(std::string_view zone) const;

    bool IsLinkLocalUnicast() const noexcept;
    bool IsLoopback() const noexcept;
    bool IsMulticast() const noexcept;
    bool IsInterfaceLocalMulticast() const noexcept;
    bool IsLinkLocalMulticast() const noexcept;
    bool IsGlobalUnicast() const noexcept;
    bool IsPrivate() const noexcept;
    bool IsUnspecified() const noexcept;

    // Prefix: the network of the first b bits; an error if b is out of
    // range, and the zero Prefix for the zero Addr.
    std::pair<netip::Prefix, error> Prefix(int b) const;

    // As16: the 16-byte form, IPv4 addresses mapped into IPv6.
    std::array<byte, 16> As16() const noexcept;
    // As4: the 4-byte form of an IPv4 or IPv4-mapped address; panics for
    // others.
    std::array<byte, 4> As4() const;
    // AsSlice: 4 or 16 bytes, nil for the zero Addr.
    slice<byte> AsSlice() const;

    // Next and Prev: the following and preceding address; the zero Addr
    // past the ends.
    Addr Next() const noexcept;
    Addr Prev() const noexcept;

    // String: "192.0.2.1", "2001:db8::1", "::ffff:192.0.2.1", with "%zone";
    // "invalid IP" for the zero Addr.
    string String() const;
    // StringExpanded: IPv6 with every field written out in full.
    string StringExpanded() const;
    // AppendTo: String() appended to b; nothing for the zero Addr.
    slice<byte> AppendTo(slice<byte> b) const;
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const { return {AppendTo(std::move(b)), nullptr}; }
    // MarshalText: as AppendTo; empty for the zero Addr.
    std::pair<slice<byte>, error> MarshalText() const;
    // UnmarshalText: ParseAddr, or the zero Addr for empty text.
    error UnmarshalText(std::span<const byte> text);
    // The binary form: 4 bytes, 16, or 16 and the zone; empty for zero.
    std::pair<slice<byte>, error> AppendBinary(slice<byte> b) const;
    std::pair<slice<byte>, error> MarshalBinary() const;
    error UnmarshalBinary(std::span<const byte> b);

    friend bool operator==(const Addr&, const Addr&) noexcept = default;

    // Internals, for the package and its tests.
    constexpr Addr(uint128 addr, const detail::addrDetail* z) noexcept : addr_(addr), z_(z) {}
    uint128 addrBits() const noexcept { return addr_; }
    const detail::addrDetail* z() const noexcept { return z_; }
    uint8 v4(uint8 i) const noexcept { return uint8(addr_.lo >> ((3 - i) * 8)); }
    uint8 v6(uint8 i) const noexcept { return uint8((i < 8 ? addr_.hi : addr_.lo) >> ((7 - i % 8) * 8)); }
    uint16 v6u16(uint8 i) const noexcept { return uint16((i < 4 ? addr_.hi : addr_.lo) >> ((3 - i % 4) * 16)); }
    bool isZero() const noexcept { return z_ == nullptr; }
    Addr withoutZone() const noexcept;
    bool hasZone() const noexcept;
    slice<byte> appendTo4(slice<byte> b) const;
    slice<byte> appendTo4In6(slice<byte> b) const;
    slice<byte> appendTo6(slice<byte> b) const;
    int marshalBinarySize() const;

private:
    friend class netip::Prefix;
    uint128 addr_;
    const detail::addrDetail* z_ = nullptr;
};

// The interned details: z0 is nullptr.
extern const detail::addrDetail* const z4;
extern const detail::addrDetail* const z6noz;
// unique.Make(addrDetail{isV6, zone}).
const detail::addrDetail* MakeAddrDetail(bool isV6, std::string_view zone);

Addr IPv6LinkLocalAllNodes() noexcept;
Addr IPv6LinkLocalAllRouters() noexcept;
Addr IPv6Loopback() noexcept;
Addr IPv6Unspecified() noexcept;
Addr IPv4Unspecified() noexcept;
Addr AddrFrom4(std::array<byte, 4> addr) noexcept;
Addr AddrFrom16(std::array<byte, 16> addr) noexcept;
// AddrFromSlice: an address from 4 or 16 bytes; false for other lengths.
std::pair<Addr, bool> AddrFromSlice(std::span<const byte> s) noexcept;

// netip.ParseAddr: "192.0.2.1", "2001:db8::1", "::ffff:192.0.2.1" or
// "fe80::1%eth0". IPv4 fields may not have leading zeros.
std::pair<Addr, error> ParseAddr(std::string_view s);
// netip.MustParseAddr: panics if s doesn't parse.
Addr MustParseAddr(std::string_view s);

// netip.AddrPort: an address and a port.
class AddrPort {
public:
    constexpr AddrPort() noexcept = default;
    constexpr AddrPort(netip::Addr ip, uint16 port) noexcept : ip_(ip), port_(port) {}
    netip::Addr Addr() const noexcept { return ip_; }
    uint16 Port() const noexcept { return port_; }
    bool IsValid() const noexcept { return ip_.IsValid(); }
    int Compare(const AddrPort& p2) const;
    // "192.0.2.1:80", "[2001:db8::1]:80"; "invalid AddrPort" when the
    // address is the zero Addr.
    string String() const;
    slice<byte> AppendTo(slice<byte> b) const;
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const { return {AppendTo(std::move(b)), nullptr}; }
    std::pair<slice<byte>, error> MarshalText() const;
    error UnmarshalText(std::span<const byte> text);
    // The address's binary form and the port, little-endian.
    std::pair<slice<byte>, error> AppendBinary(slice<byte> b) const;
    std::pair<slice<byte>, error> MarshalBinary() const;
    error UnmarshalBinary(std::span<const byte> b);
    friend bool operator==(const AddrPort&, const AddrPort&) noexcept = default;

private:
    netip::Addr ip_;
    uint16 port_ = 0;
};

AddrPort AddrPortFrom(Addr ip, uint16 port) noexcept;
// netip.ParseAddrPort: "192.0.2.1:80" or "[2001:db8::1]:80".
std::pair<AddrPort, error> ParseAddrPort(std::string_view s);
AddrPort MustParseAddrPort(std::string_view s);

// netip.Prefix: an address and a prefix length, such as 192.0.2.0/24.
// The address isn't masked; Masked() does that.
class Prefix {
public:
    constexpr Prefix() noexcept = default;
    // Prefix{ip, bits} as a Go composite literal in the package: no checks.
    constexpr Prefix(netip::Addr ip, uint8 bitsPlusOne) noexcept : ip_(ip), bitsPlusOne_(bitsPlusOne) {}
    netip::Addr Addr() const noexcept { return ip_; }
    // Bits: the prefix length, -1 if the Prefix is invalid.
    int Bits() const noexcept { return int(bitsPlusOne_) - 1; }
    bool IsValid() const noexcept { return bitsPlusOne_ > 0; }
    bool IsSingleIP() const noexcept { return IsValid() && Bits() == ip_.BitLen(); }
    int Compare(const Prefix& p2) const;
    // Masked: the prefix with the bits after it zeroed.
    Prefix Masked() const;
    // Contains: whether ip is in the network; zoned addresses never are.
    bool Contains(const netip::Addr& ip) const noexcept;
    bool Overlaps(const Prefix& o) const;
    // "192.0.2.0/24"; "invalid Prefix".
    string String() const;
    slice<byte> AppendTo(slice<byte> b) const;
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const { return {AppendTo(std::move(b)), nullptr}; }
    std::pair<slice<byte>, error> MarshalText() const;
    error UnmarshalText(std::span<const byte> text);
    std::pair<slice<byte>, error> AppendBinary(slice<byte> b) const;
    std::pair<slice<byte>, error> MarshalBinary() const;
    error UnmarshalBinary(std::span<const byte> b);
    friend bool operator==(const Prefix&, const Prefix&) noexcept = default;

    bool isZero() const noexcept { return *this == Prefix{}; }

private:
    netip::Addr ip_;
    uint8 bitsPlusOne_ = 0;
};

// netip.PrefixFrom: no checks beyond making an out-of-range bits invalid;
// the zone is dropped.
Prefix PrefixFrom(Addr ip, int bits);
// netip.ParsePrefix: "192.0.2.0/24", "2001:db8::/32". No zones.
std::pair<Prefix, error> ParsePrefix(std::string_view s);
Prefix MustParsePrefix(std::string_view s);

}  // namespace gopp::netip

template <>
struct std::hash<gopp::netip::Addr> {
    std::size_t operator()(const gopp::netip::Addr& a) const noexcept {
        auto b = a.addrBits();
        std::size_t h = std::hash<gopp::uint64>()(b.hi) * 31 + std::hash<gopp::uint64>()(b.lo);
        return h * 31 + std::hash<const void*>()(a.z());
    }
};

template <>
struct std::hash<gopp::netip::AddrPort> {
    std::size_t operator()(const gopp::netip::AddrPort& a) const noexcept {
        return std::hash<gopp::netip::Addr>()(a.Addr()) * 31 + a.Port();
    }
};

template <>
struct std::hash<gopp::netip::Prefix> {
    std::size_t operator()(const gopp::netip::Prefix& p) const noexcept {
        return std::hash<gopp::netip::Addr>()(p.Addr()) * 31 + std::size_t(p.Bits() + 1);
    }
};

// Addr, AddrPort and Prefix format as their String().
template <typename T>
    requires std::is_same_v<T, gopp::netip::Addr> || std::is_same_v<T, gopp::netip::AddrPort> ||
             std::is_same_v<T, gopp::netip::Prefix>
struct std::formatter<T> : std::formatter<std::string_view> {
    auto format(const T& v, std::format_context& ctx) const {
        return std::formatter<std::string_view>::format(v.String().view(), ctx);
    }
};
