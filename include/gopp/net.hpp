// The net package: TCP, UDP and Unix domain sockets on the runtime's
// network poller, IP addresses, and address resolution.
//
// Reads, writes, accepts and dials park the calling fiber until the
// socket is ready; the thread runs other fibers meanwhile.
//
//     auto [ln, err] = net::Listen("tcp", ":8080");
//     auto [c, err] = net::Dial("udp", "127.0.0.1:53");
//     auto [pc, err] = net::ListenPacket("udp", ":0");
//     auto [n, addr, err] = pc->ReadFrom(buf);
//     auto [ln, err] = net::Listen("unix", "/run/app.sock");
//     net::Dialer d{.Timeout = 5 * time::Second};
//     auto [c, err] = d.DialContext(ctx, "tcp", "example.com:443");
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/net/netip.hpp>
#include <gopp/os.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <atomic>
#include <format>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
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
    // The address as a netip::AddrPort; an invalid one if IP isn't 4 or 16
    // bytes.
    netip::AddrPort AddrPort() const;
};

// net.TCPAddrFromAddrPort
ptr<TCPAddr> TCPAddrFromAddrPort(netip::AddrPort addr);

// net.UDPAddr
struct UDPAddr final : Addr {
    net::IP IP;
    int Port = 0;
    string Zone;  // IPv6 scoped addressing zone
    UDPAddr() = default;
    UDPAddr(net::IP ip, int port, string zone = {}) : IP(std::move(ip)), Port(port), Zone(std::move(zone)) {}
    string Network() const override { return string::from_static("udp"); }
    string String() const override;
    netip::AddrPort AddrPort() const;
};

// net.UDPAddrFromAddrPort
ptr<UDPAddr> UDPAddrFromAddrPort(netip::AddrPort addr);

// net.UnixAddr: the path of a Unix domain socket, or "@name" for one in
// Linux's abstract namespace (and "@" for an unnamed one), and its network:
// "unix", "unixgram" or "unixpacket".
struct UnixAddr final : Addr {
    string Name;
    string Net;
    UnixAddr() = default;
    UnixAddr(string name, string net) : Name(std::move(name)), Net(std::move(net)) {}
    string Network() const override { return Net; }
    string String() const override { return Name; }
};

// net.ResolveUnixAddr: network is "unix", "unixgram" or "unixpacket".
std::pair<ptr<UnixAddr>, error> ResolveUnixAddr(std::string_view network, std::string_view address);

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
    gopp::error UnwrapErr;  // the error behind it, if any
    string Err;
    string Name;
    string Server;
    bool IsTimeout = false;
    bool IsTemporary = false;
    bool IsNotFound = false;
    string Error() const override;
    bool Timeout() const noexcept override { return IsTimeout; }
    const error_interface* Unwrap() const noexcept override { return UnwrapErr.get(); }
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
class Buffers;

// The methods TCPConn, UDPConn and UnixConn share (Go's net.conn).
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
    // A copy of the socket as a File; closing one doesn't close the other.
    std::pair<ptr<os::File>, error> File();
    // The socket's descriptor, for options go++ doesn't wrap.
    std::pair<ptr<syscall::RawConn>, error> SyscallConn();

    // Writes the buffers with writev(2) (Buffers::WriteTo uses it).
    std::pair<int64, error> writeBuffers(Buffers& v);
    const ptr<netfd>& internal_fd() const noexcept { return fd_; }

protected:
    ptr<netfd> fd_;
};

// net.KeepAliveConfig: TCP keep-alive probes. Zero fields take the
// defaults (15s idle, 15s interval, 9 probes); negative ones leave the
// system's settings.
struct KeepAliveConfig {
    bool Enable = false;
    time::Duration Idle{};
    time::Duration Interval{};
    int Count = 0;
};

// net.TCPConn. io::Copy to it from another TCP or Unix stream connection
// uses splice(2), and from an os::File sendfile(2); from it to an os::File,
// splice(2).
class TCPConn final : public conn_base, public io::ReaderFrom, public io::WriterTo {
public:
    using conn_base::conn_base;
    // ReadFrom and WriteTo: io::ReaderFrom and io::WriterTo.
    std::pair<int64, error> ReadFrom(io::Reader& r) override;
    std::pair<int64, error> WriteTo(io::Writer& w) override;
    // Shut down reading or writing (shutdown(2)).
    error CloseRead();
    error CloseWrite();
    // SO_LINGER: < 0 the default, 0 to drop unsent data on Close, > 0 to
    // wait up to sec seconds for it to be sent.
    error SetLinger(int sec);
    error SetKeepAlive(bool keepalive);
    // The idle time before keep-alive probes.
    error SetKeepAlivePeriod(time::Duration d);
    error SetKeepAliveConfig(const KeepAliveConfig& config);
    error SetNoDelay(bool noDelay);
    // Whether the connection uses Multipath TCP.
    std::pair<bool, error> MultipathTCP();
};

// net.TCPListener
class TCPListener final : public Listener {
public:
    TCPListener(ptr<netfd> fd, KeepAliveConfig keepalive, time::Duration keepAliveIdle);
    ~TCPListener() override;

    std::pair<ptr<TCPConn>, error> AcceptTCP();
    std::pair<ptr<Conn>, error> Accept() override;
    error Close() override;
    ptr<net::Addr> Addr() const override;
    error SetDeadline(time::Time t);
    // A copy of the listener's socket as a File.
    std::pair<ptr<os::File>, error> File();
    // Control works; Read and Write fail with EINVAL.
    std::pair<ptr<syscall::RawConn>, error> SyscallConn();

private:
    ptr<netfd> fd_;
    KeepAliveConfig keepalive_;
    time::Duration keepAliveIdle_;
};

// net.UDPConn: both a Conn, for a socket connected to a peer (DialUDP),
// and a PacketConn.
class UDPConn final : public conn_base, public PacketConn {
public:
    using conn_base::conn_base;

    std::tuple<isize, ptr<UDPAddr>, error> ReadFromUDP(std::span<byte> b);
    std::tuple<isize, ptr<net::Addr>, error> ReadFrom(std::span<byte> b) override;
    std::tuple<isize, netip::AddrPort, error> ReadFromUDPAddrPort(std::span<byte> b);
    // Reads a packet and its out-of-band data (control messages): returns
    // n, oobn, the flags of recvmsg(2), the sender, and an error.
    std::tuple<isize, isize, int, ptr<UDPAddr>, error> ReadMsgUDP(std::span<byte> b, std::span<byte> oob);
    std::tuple<isize, isize, int, netip::AddrPort, error> ReadMsgUDPAddrPort(std::span<byte> b, std::span<byte> oob);
    std::pair<isize, error> WriteToUDP(std::span<const byte> b, const ptr<UDPAddr>& addr);
    std::pair<isize, error> WriteToUDPAddrPort(std::span<const byte> b, netip::AddrPort addr);
    std::pair<isize, error> WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) override;
    // Writes a packet with out-of-band data to addr, or, on a connected
    // socket, with addr nil, to its peer. Returns n, oobn and an error.
    std::tuple<isize, isize, error> WriteMsgUDP(std::span<const byte> b, std::span<const byte> oob,
                                                const ptr<UDPAddr>& addr);
    std::tuple<isize, isize, error> WriteMsgUDPAddrPort(std::span<const byte> b, std::span<const byte> oob,
                                                        netip::AddrPort addr);

    // Conn and PacketConn both have these.
    error Close() override { return conn_base::Close(); }
    ptr<net::Addr> LocalAddr() const override { return conn_base::LocalAddr(); }
    error SetDeadline(time::Time t) override { return conn_base::SetDeadline(t); }
    error SetReadDeadline(time::Time t) override { return conn_base::SetReadDeadline(t); }
    error SetWriteDeadline(time::Time t) override { return conn_base::SetWriteDeadline(t); }
};

// net.UnixConn: a Unix domain socket, stream ("unix"), datagram
// ("unixgram") or sequenced packet ("unixpacket").
class UnixConn final : public conn_base, public PacketConn {
public:
    using conn_base::conn_base;

    // Shut down reading or writing (shutdown(2)).
    error CloseRead();
    error CloseWrite();

    std::tuple<isize, ptr<UnixAddr>, error> ReadFromUnix(std::span<byte> b);
    std::tuple<isize, ptr<net::Addr>, error> ReadFrom(std::span<byte> b) override;
    // Reads a message and its out-of-band data, such as the descriptors of
    // an SCM_RIGHTS message (syscall::ParseUnixRights); they arrive
    // close-on-exec. Returns n, oobn, flags, the sender, and an error.
    std::tuple<isize, isize, int, ptr<UnixAddr>, error> ReadMsgUnix(std::span<byte> b, std::span<byte> oob);
    std::pair<isize, error> WriteToUnix(std::span<const byte> b, const ptr<UnixAddr>& addr);
    std::pair<isize, error> WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) override;
    // Writes a message with out-of-band data (syscall::UnixRights) to addr,
    // or, with addr nil, to the peer. Returns n, oobn and an error.
    std::tuple<isize, isize, error> WriteMsgUnix(std::span<const byte> b, std::span<const byte> oob,
                                                 const ptr<UnixAddr>& addr);

    error Close() override { return conn_base::Close(); }
    ptr<net::Addr> LocalAddr() const override { return conn_base::LocalAddr(); }
    error SetDeadline(time::Time t) override { return conn_base::SetDeadline(t); }
    error SetReadDeadline(time::Time t) override { return conn_base::SetReadDeadline(t); }
    error SetWriteDeadline(time::Time t) override { return conn_base::SetWriteDeadline(t); }
};

// net.UnixListener. Closing it removes its socket file, unless
// SetUnlinkOnClose(false) said otherwise or it came from FileListener.
class UnixListener final : public Listener {
public:
    UnixListener(ptr<netfd> fd, string path, bool unlink);
    ~UnixListener() override;

    std::pair<ptr<UnixConn>, error> AcceptUnix();
    std::pair<ptr<Conn>, error> Accept() override;
    error Close() override;
    ptr<net::Addr> Addr() const override;
    error SetDeadline(time::Time t);
    void SetUnlinkOnClose(bool unlink) { unlink_ = unlink; }
    // A copy of the listener's socket as a File.
    std::pair<ptr<os::File>, error> File();
    std::pair<ptr<syscall::RawConn>, error> SyscallConn();

private:
    ptr<netfd> fd_;
    string path_;
    bool unlink_;
    std::atomic<bool> unlinked_{false};
};

// ---------------------------------------------------------------------------
// Dialing and listening

// The callbacks of Dialer and ListenConfig that see the socket before it
// connects or binds: network ("tcp4", "udp6", "unix", ...), address, and
// the socket.
using ControlFunc = std::function<error(std::string_view network, std::string_view address, syscall::RawConn& c)>;
using ControlContextFunc = std::function<error(const ptr<context::Context>& ctx, std::string_view network,
                                               std::string_view address, syscall::RawConn& c)>;

// net.Dialer: options for connecting. The zero Dialer is what Dial uses.
//
//     net::Dialer d{.Timeout = 5 * time::Second, .KeepAlive = 30 * time::Second};
//     auto [c, err] = d.DialContext(ctx, "tcp", "example.com:443");
struct Dialer {
    // How long a dial may take, name resolution included; zero for no
    // limit (the system's still applies). With several addresses, each
    // gets its share.
    time::Duration Timeout{};
    // When dials fail; zero for none. The earlier of this, Timeout and
    // the context's deadline counts.
    time::Time Deadline{};
    // The local address to dial from; nil picks one.
    ptr<net::Addr> LocalAddr{};
    // Deprecated: Happy Eyeballs is on by default; FallbackDelay < 0 turns
    // it off.
    bool DualStack = false;
    // How long a "tcp" dial waits for the first address family before
    // starting the other, as RFC 6555 (Happy Eyeballs) has it: zero is
    // 300ms, negative turns fallback off.
    time::Duration FallbackDelay{};
    // The keep-alive idle time of TCP connections: zero is 15s, negative
    // turns keep-alives off. Ignored when KeepAliveConfig.Enable.
    time::Duration KeepAlive{};
    net::KeepAliveConfig KeepAliveConfig{};
    // Deprecated: closing it cancels the dial; use DialContext.
    chan<empty> Cancel{};
    // Called with the socket before it connects.
    ControlFunc Control{};
    // As Control, with the dial's context; it takes precedence.
    ControlContextFunc ControlContext{};

    // Dial: DialContext with context::Background().
    std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address);
    // DialContext: connects to address on network, as net::Dial; the dial
    // fails if ctx is canceled before it connects. "tcp" addresses of both
    // families race as RFC 6555 says.
    std::pair<ptr<Conn>, error> DialContext(const ptr<context::Context>& ctx, std::string_view network,
                                            std::string_view address);
    // DialTCP, DialUDP and DialUnix: as the functions, with the Dialer's
    // options; an invalid laddr picks one.
    std::pair<ptr<TCPConn>, error> DialTCP(const ptr<context::Context>& ctx, std::string_view network,
                                           netip::AddrPort laddr, netip::AddrPort raddr);
    std::pair<ptr<UDPConn>, error> DialUDP(const ptr<context::Context>& ctx, std::string_view network,
                                           netip::AddrPort laddr, netip::AddrPort raddr);
    std::pair<ptr<UnixConn>, error> DialUnix(const ptr<context::Context>& ctx, std::string_view network,
                                             const ptr<UnixAddr>& laddr, const ptr<UnixAddr>& raddr);
    // Whether "tcp" dials try Multipath TCP first; off by default.
    bool MultipathTCP() const;
    void SetMultipathTCP(bool use);

    // 0: the default, 1: on, 2: off.
    uint8 mptcpStatus = 0;
};

// net.ListenConfig: options for listening. The zero ListenConfig is what
// Listen and ListenPacket use.
struct ListenConfig {
    // Called with the socket before it binds.
    ControlFunc Control{};
    // The keep-alive idle time of accepted TCP connections: zero is 15s,
    // negative turns keep-alives off. Ignored when KeepAliveConfig.Enable.
    time::Duration KeepAlive{};
    net::KeepAliveConfig KeepAliveConfig{};

    std::pair<ptr<net::Listener>, error> Listen(const ptr<context::Context>& ctx, std::string_view network,
                                                std::string_view address);
    std::pair<ptr<net::PacketConn>, error> ListenPacket(const ptr<context::Context>& ctx, std::string_view network,
                                                       std::string_view address);
    // Whether TCP listeners use Multipath TCP where the kernel has it; on
    // by default, as in Go; connections fall back to TCP.
    bool MultipathTCP() const;
    void SetMultipathTCP(bool use);

    uint8 mptcpStatus = 0;
};

// net.Listen: network is "tcp", "tcp4" or "tcp6", with an address
// "host:port", ":port" or "[ipv6]:port"; or "unix" or "unixpacket", with
// a socket path.
std::pair<ptr<Listener>, error> Listen(std::string_view network, std::string_view address);

// net.ListenPacket: network is "udp", "udp4", "udp6" or "unixgram".
std::pair<ptr<PacketConn>, error> ListenPacket(std::string_view network, std::string_view address);

// net.Dial: network is "tcp", "tcp4", "tcp6", "udp", "udp4", "udp6",
// "unix", "unixgram" or "unixpacket".
std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address);

// net.DialTimeout
std::pair<ptr<Conn>, error> DialTimeout(std::string_view network, std::string_view address, time::Duration timeout);

// net.ListenTCP: listens on laddr; nil, a nil IP or port 0 pick one.
std::pair<ptr<TCPListener>, error> ListenTCP(std::string_view network, const ptr<TCPAddr>& laddr);

// net.DialTCP: connects to raddr, from laddr if it isn't nil.
std::pair<ptr<TCPConn>, error> DialTCP(std::string_view network, const ptr<TCPAddr>& laddr, const ptr<TCPAddr>& raddr);

// net.ListenUDP: listens on laddr; nil, or a nil IP or port 0, picks one.
std::pair<ptr<UDPConn>, error> ListenUDP(std::string_view network, const ptr<UDPAddr>& laddr);

// net.DialUDP: a UDP socket connected to raddr, bound to laddr if it
// isn't nil.
std::pair<ptr<UDPConn>, error> DialUDP(std::string_view network, const ptr<UDPAddr>& laddr, const ptr<UDPAddr>& raddr);

// net.ListenUnix: network is "unix" or "unixpacket". An empty laddr Name
// binds to an abstract name the kernel picks (Linux's autobind).
std::pair<ptr<UnixListener>, error> ListenUnix(std::string_view network, const ptr<UnixAddr>& laddr);

// net.ListenUnixgram: network is "unixgram".
std::pair<ptr<UnixConn>, error> ListenUnixgram(std::string_view network, const ptr<UnixAddr>& laddr);

// net.DialUnix: connects to raddr, bound to laddr if it isn't nil.
std::pair<ptr<UnixConn>, error> DialUnix(std::string_view network, const ptr<UnixAddr>& laddr,
                                         const ptr<UnixAddr>& raddr);

// net.FileConn, net.FileListener and net.FilePacketConn: a copy of the
// socket in f, which stays open.
std::pair<ptr<Conn>, error> FileConn(const ptr<os::File>& f);
std::pair<ptr<Listener>, error> FileListener(const ptr<os::File>& f);
std::pair<ptr<PacketConn>, error> FilePacketConn(const ptr<os::File>& f);

// net.SplitHostPort
std::tuple<string, string, error> SplitHostPort(std::string_view hostport);

// net.JoinHostPort
string JoinHostPort(std::string_view host, std::string_view port);

// ---------------------------------------------------------------------------
// Buffers and Pipe

// net.Buffers: byte slices to write together; to a connection that's one
// writev(2). Writing or reading consumes them.
class Buffers : public std::vector<slice<byte>>, public io::Reader, public io::WriterTo {
public:
    using std::vector<slice<byte>>::vector;
    // WriteTo writes the buffers to w and empties them.
    std::pair<int64, error> WriteTo(io::Writer& w) override;
    // Read reads from the buffers, consuming what it read.
    std::pair<isize, error> Read(std::span<byte> p) override;
    // Drops the first n bytes.
    void consume(int64 n);
};

// net.Pipe: a synchronous, in-memory, full-duplex connection: each Write
// blocks until reads on the other end consumed it all. Deadlines work.
std::pair<ptr<Conn>, ptr<Conn>> Pipe();

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
