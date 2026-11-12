// The net package: TCP and UDP on the runtime's network poller, IP
// addresses, and address resolution.
//
// Reads, writes, accepts and dials park the calling fiber until the
// socket is ready; the thread runs other fibers meanwhile.
//
//     auto [ln, err] = net::Listen("tcp", ":8080");
//     auto [c, err] = net::Dial("udp", "127.0.0.1:53");
//     auto [pc, err] = net::ListenPacket("udp", ":0");
//     auto [n, addr, err] = pc->ReadFrom(buf);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <format>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#endif

namespace gopp::net {

// ---------------------------------------------------------------------------
// IP addresses (ip.go)

inline constexpr int IPv4len = 4;
inline constexpr int IPv6len = 16;

// net.IP: an IP address as a byte slice, 4 bytes (IPv4) or 16 (IPv6, or
// IPv4 in IPv6). A nil (empty) IP stands for no address, as in Go.
class IP : public slice<byte> {
public:
    using slice<byte>::slice;
    IP() = default;
    IP(slice<byte> s) noexcept : slice<byte>(std::move(s)) {}

    // To4 is the 4-byte form of an IPv4 address, nil if ip isn't one.
    IP To4() const;
    // To16 is the 16-byte form, nil if ip isn't an address.
    IP To16() const;
    bool Equal(const IP& x) const;
    // The default mask of an IPv4 address's class; nil otherwise.
    slice<byte> DefaultMask() const;
    IP Mask(std::span<const byte> mask) const;

    bool IsUnspecified() const;
    bool IsLoopback() const;
    bool IsPrivate() const;
    bool IsMulticast() const;
    bool IsInterfaceLocalMulticast() const;
    bool IsLinkLocalMulticast() const;
    bool IsLinkLocalUnicast() const;
    bool IsGlobalUnicast() const;

    // "192.0.2.1", "2001:db8::1" (RFC 5952), "<nil>", or "?" and hex for a
    // byte sequence of another length.
    string String() const;
    std::pair<slice<byte>, error> MarshalText() const;
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const;
    error UnmarshalText(std::span<const byte> text);
};

// net.IPv4: the 16-byte form of a.b.c.d.
IP IPv4(byte a, byte b, byte c, byte d);
// net.ParseIP: an IPv4 or IPv6 address in text, in 16-byte form; nil if s
// isn't one. Zones aren't allowed.
IP ParseIP(std::string_view s);

extern const IP IPv4bcast;
extern const IP IPv4allsys;
extern const IP IPv4allrouter;
extern const IP IPv4zero;
extern const IP IPv6zero;
extern const IP IPv6unspecified;
extern const IP IPv6loopback;
extern const IP IPv6interfacelocalallnodes;
extern const IP IPv6linklocalallnodes;
extern const IP IPv6linklocalallrouters;

// net.IPMask
class IPMask : public slice<byte> {
public:
    using slice<byte>::slice;
    IPMask() = default;
    IPMask(slice<byte> s) noexcept : slice<byte>(std::move(s)) {}
    // The number of leading ones and of bits; 0, 0 if the mask isn't
    // canonical (ones then zeros).
    std::pair<int, int> Size() const;
    string String() const;
};

IPMask IPv4Mask(byte a, byte b, byte c, byte d);
// net.CIDRMask: ones 1 bits then zeros, bits long; nil if that's invalid.
IPMask CIDRMask(int ones, int bits);

// ---------------------------------------------------------------------------
// Addresses

// net.Addr: a network end point address.
struct Addr : public virtual object {
    virtual string Network() const = 0;  // "tcp", "udp", ...
    virtual string String() const = 0;   // "192.0.2.1:25", "[2001:db8::1]:80"
};

// net.IPNet: an IP network.
struct IPNet final : Addr {
    net::IP IP;
    IPMask Mask;
    IPNet() = default;
    IPNet(net::IP ip, IPMask mask) : IP(std::move(ip)), Mask(std::move(mask)) {}
    bool Contains(const net::IP& ip) const;
    string Network() const override { return string::from_static("ip+net"); }
    string String() const override;
};

// net.ParseCIDR: "192.0.2.1/24" as its IP and network.
std::tuple<IP, ptr<IPNet>, error> ParseCIDR(std::string_view s);

// net.TCPAddr
struct TCPAddr final : Addr {
    net::IP IP;
    int Port = 0;
    string Zone;  // IPv6 scoped addressing zone
    TCPAddr() = default;
    TCPAddr(net::IP ip, int port, string zone = {}) : IP(std::move(ip)), Port(port), Zone(std::move(zone)) {}
    string Network() const override { return string::from_static("tcp"); }
    string String() const override;
};

// net.UDPAddr
struct UDPAddr final : Addr {
    net::IP IP;
    int Port = 0;
    string Zone;  // IPv6 scoped addressing zone
    UDPAddr() = default;
    UDPAddr(net::IP ip, int port, string zone = {}) : IP(std::move(ip)), Port(port), Zone(std::move(zone)) {}
    string Network() const override { return string::from_static("udp"); }
    string String() const override;
};

// net.ResolveTCPAddr: network is "tcp", "tcp4" or "tcp6"; address is
// "host:port", where host may be a name, and port a service name.
std::pair<ptr<TCPAddr>, error> ResolveTCPAddr(std::string_view network, std::string_view address);
// net.ResolveUDPAddr: likewise for "udp", "udp4" and "udp6".
std::pair<ptr<UDPAddr>, error> ResolveUDPAddr(std::string_view network, std::string_view address);

// net.LookupPort: the port of a service ("http", "domain") or number.
std::pair<int, error> LookupPort(std::string_view network, std::string_view service);

// ---------------------------------------------------------------------------
// Errors

// net.ErrClosed: "use of closed network connection".
extern const error ErrClosed;
// os.ErrDeadlineExceeded: "i/o timeout"; Timeout() is true.
using os::ErrDeadlineExceeded;
// net.ErrWriteToConnected: WriteTo on a connected socket.
extern const error ErrWriteToConnected;

// net.OpError: the operation, network and addresses of a failed call.
struct OpError : error_interface {
    string Op;
    string Net;
    ptr<net::Addr> Source;
    ptr<net::Addr> Addr;
    gopp::error Err;
    string Error() const override;
    const error_interface* Unwrap() const noexcept override { return Err.get(); }
    bool Timeout() const noexcept override { return Err && Err->Timeout(); }
};

// net.AddrError
struct AddrError : error_interface {
    string Err;
    string Addr;
    AddrError(string err, string addr) : Err(std::move(err)), Addr(std::move(addr)) {}
    string Error() const override;
};

// net.UnknownNetworkError
struct UnknownNetworkError : error_interface {
    string Net;
    explicit UnknownNetworkError(string net) : Net(std::move(net)) {}
    string Error() const override { return "unknown network " + Net; }
};

// net.InvalidAddrError
struct InvalidAddrError : error_interface {
    string Msg;
    explicit InvalidAddrError(string msg) : Msg(std::move(msg)) {}
    string Error() const override { return Msg; }
};

// net.ParseError: a string that isn't the expected kind of address.
struct ParseError : error_interface {
    string Type;  // "IP address", "CIDR address"
    string Text;
    ParseError(string type, string text) : Type(std::move(type)), Text(std::move(text)) {}
    string Error() const override { return "invalid " + Type + ": " + Text; }
};

// net.DNSError
struct DNSError : error_interface {
    string Err;
    string Name;
    string Server;
    bool IsTimeout = false;
    bool IsTemporary = false;
    bool IsNotFound = false;
    string Error() const override;
    bool Timeout() const noexcept override { return IsTimeout; }
};

// syscall.Errno, also known as net::Errno.
using syscall::Errno;
using syscall::errno_error;

// ---------------------------------------------------------------------------
// Connections

// net.Conn: a stream-oriented connection.
struct Conn : public io::Reader, public io::Writer, public io::Closer {
    virtual ptr<net::Addr> LocalAddr() const = 0;
    // nil for an unconnected socket.
    virtual ptr<net::Addr> RemoteAddr() const = 0;
    // A zero Time means no deadline.
    virtual error SetDeadline(time::Time t) = 0;
    virtual error SetReadDeadline(time::Time t) = 0;
    virtual error SetWriteDeadline(time::Time t) = 0;
};

// net.PacketConn: a packet-oriented connection.
struct PacketConn : public virtual object {
    // Reads a packet into p, returning its size, up to len(p), and sender.
    virtual std::tuple<isize, ptr<net::Addr>, error> ReadFrom(std::span<byte> p) = 0;
    // Writes a packet to addr.
    virtual std::pair<isize, error> WriteTo(std::span<const byte> p, const ptr<net::Addr>& addr) = 0;
    virtual error Close() = 0;
    virtual ptr<net::Addr> LocalAddr() const = 0;
    virtual error SetDeadline(time::Time t) = 0;
    virtual error SetReadDeadline(time::Time t) = 0;
    virtual error SetWriteDeadline(time::Time t) = 0;
};

// net.Listener
struct Listener : public io::Closer {
    virtual std::pair<ptr<Conn>, error> Accept() = 0;
    virtual ptr<net::Addr> Addr() const = 0;
};

class netfd;

// The methods TCPConn and UDPConn share (Go's net.conn).
class conn_base : public Conn {
public:
    explicit conn_base(ptr<netfd> fd);
    // A dropped connection is closed, as a garbage-collected one would be
    // in Go eventually.
    ~conn_base() override;

    std::pair<isize, error> Read(std::span<byte> p) override;
    std::pair<isize, error> Write(std::span<const byte> p) override;
    error Close() override;
    ptr<net::Addr> LocalAddr() const override;
    ptr<net::Addr> RemoteAddr() const override;
    error SetDeadline(time::Time t) override;
    error SetReadDeadline(time::Time t) override;
    error SetWriteDeadline(time::Time t) override;
    // The socket's buffer sizes (SO_RCVBUF, SO_SNDBUF).
    error SetReadBuffer(int bytes);
    error SetWriteBuffer(int bytes);

protected:
    ptr<netfd> fd_;
};

// net.TCPConn
class TCPConn final : public conn_base {
public:
    using conn_base::conn_base;
    error CloseRead();
    error CloseWrite();
    error SetNoDelay(bool on);
    error SetKeepAlive(bool on);
};

// net.UDPConn: both a Conn, for a socket connected to a peer (DialUDP),
// and a PacketConn.
class UDPConn final : public conn_base, public PacketConn {
public:
    using conn_base::conn_base;

    std::tuple<isize, ptr<UDPAddr>, error> ReadFromUDP(std::span<byte> b);
    std::tuple<isize, ptr<net::Addr>, error> ReadFrom(std::span<byte> b) override;
    // Reads a packet and its out-of-band data (control messages): returns
    // n, oobn, the flags of recvmsg(2), the sender, and an error.
    std::tuple<isize, isize, int, ptr<UDPAddr>, error> ReadMsgUDP(std::span<byte> b, std::span<byte> oob);
    std::pair<isize, error> WriteToUDP(std::span<const byte> b, const ptr<UDPAddr>& addr);
    std::pair<isize, error> WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) override;
    // Writes a packet with out-of-band data to addr, or, on a connected
    // socket, with addr nil, to its peer. Returns n, oobn and an error.
    std::tuple<isize, isize, error> WriteMsgUDP(std::span<const byte> b, std::span<const byte> oob,
                                                const ptr<UDPAddr>& addr);

    // Conn and PacketConn both have these.
    error Close() override { return conn_base::Close(); }
    ptr<net::Addr> LocalAddr() const override { return conn_base::LocalAddr(); }
    error SetDeadline(time::Time t) override { return conn_base::SetDeadline(t); }
    error SetReadDeadline(time::Time t) override { return conn_base::SetReadDeadline(t); }
    error SetWriteDeadline(time::Time t) override { return conn_base::SetWriteDeadline(t); }
};

// net.Listen: network is "tcp", "tcp4" or "tcp6"; address is
// "host:port", ":port" or "[ipv6]:port".
std::pair<ptr<Listener>, error> Listen(std::string_view network, std::string_view address);

// net.ListenPacket: network is "udp", "udp4" or "udp6".
std::pair<ptr<PacketConn>, error> ListenPacket(std::string_view network, std::string_view address);

// net.ListenUDP: listens on laddr; nil, or a nil IP or port 0, picks one.
std::pair<ptr<UDPConn>, error> ListenUDP(std::string_view network, const ptr<UDPAddr>& laddr);

// net.DialUDP: a UDP socket connected to raddr, bound to laddr if it
// isn't nil.
std::pair<ptr<UDPConn>, error> DialUDP(std::string_view network, const ptr<UDPAddr>& laddr, const ptr<UDPAddr>& raddr);

// net.Dial: network is "tcp", "tcp4", "tcp6", "udp", "udp4" or "udp6".
std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address);

// net.DialTimeout
std::pair<ptr<Conn>, error> DialTimeout(std::string_view network, std::string_view address, time::Duration timeout);

// net.SplitHostPort
std::tuple<string, string, error> SplitHostPort(std::string_view hostport);

// net.JoinHostPort
string JoinHostPort(std::string_view host, std::string_view port);

}  // namespace gopp::net

template <>
struct std::formatter<gopp::net::IP> : std::formatter<std::string_view> {
    auto format(const gopp::net::IP& ip, std::format_context& ctx) const {
        return std::formatter<std::string_view>::format(ip.String().view(), ctx);
    }
};

// Addresses format as their String(), and nil as "<nil>".
template <typename T>
    requires std::is_base_of_v<gopp::net::Addr, T>
struct std::formatter<gopp::ptr<T>> : std::formatter<std::string_view> {
    auto format(const gopp::ptr<T>& a, std::format_context& ctx) const {
        if (!a) return std::formatter<std::string_view>::format("<nil>", ctx);
        return std::formatter<std::string_view>::format(a->String().view(), ctx);
    }
};
