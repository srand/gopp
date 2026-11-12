// The net package: TCP on the runtime's network poller.
//
// Reads, writes, accepts and dials park the calling fiber until the
// socket is ready; the thread runs other fibers meanwhile.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <string_view>
#include <tuple>
#include <utility>
#endif

namespace gopp::net {

// net.ErrClosed: "use of closed network connection".
extern const error ErrClosed;
// os.ErrDeadlineExceeded: "i/o timeout"; timeout() is true.
extern const error ErrDeadlineExceeded;

// net.OpError: the operation, network and addresses of a failed call.
struct OpError : error_interface {
    string Op;
    string Net;
    string Source;
    string Addr;
    gopp::error Err;
    string Error() const override;
    const error_interface* Unwrap() const noexcept override { return Err.get(); }
    bool Timeout() const noexcept override { return Err && Err->Timeout(); }
};

// An errno from a system call (syscall.Errno).
struct Errno : error_interface {
    int Code;
    explicit Errno(int c) : Code(c) {}
    string Error() const override;
    bool Timeout() const noexcept override;
};
gopp::error errno_error(int code);

// net.Conn
struct Conn : public io::Reader, public io::Writer, public io::Closer {
    virtual string LocalAddr() const = 0;
    virtual string RemoteAddr() const = 0;
    // A zero Time means no deadline.
    virtual error SetDeadline(time::Time t) = 0;
    virtual error SetReadDeadline(time::Time t) = 0;
    virtual error SetWriteDeadline(time::Time t) = 0;
};

// net.Listener
struct Listener : public io::Closer {
    virtual std::pair<ptr<Conn>, error> Accept() = 0;
    virtual string Addr() const = 0;
};

class netfd;

// net.TCPConn
class TCPConn final : public Conn {
public:
    explicit TCPConn(ptr<netfd> fd);
    ~TCPConn() override;

    std::pair<isize, error> Read(std::span<byte> p) override;
    std::pair<isize, error> Write(std::span<const byte> p) override;
    error Close() override;
    string LocalAddr() const override;
    string RemoteAddr() const override;
    error SetDeadline(time::Time t) override;
    error SetReadDeadline(time::Time t) override;
    error SetWriteDeadline(time::Time t) override;

    error CloseWrite();
    error SetNoDelay(bool on);
    error SetKeepAlive(bool on);

private:
    ptr<netfd> fd_;
};

// net.Listen: network is "tcp", "tcp4" or "tcp6"; address is
// "host:port", ":port" or "[ipv6]:port".
std::pair<ptr<Listener>, error> Listen(std::string_view network, std::string_view address);

// net.Dial
std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address);

// net.DialTimeout
std::pair<ptr<Conn>, error> DialTimeout(std::string_view network, std::string_view address, time::Duration timeout);

// net.SplitHostPort
std::tuple<string, string, error> SplitHostPort(std::string_view hostport);

// net.JoinHostPort
string JoinHostPort(std::string_view host, std::string_view port);

}  // namespace gopp::net
