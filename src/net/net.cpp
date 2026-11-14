// Connections and errors, after Go's net/net.go, file_unix.go and the
// address conversions of ipsock_posix.go and unixsock_posix.go.
#include "dial.hpp"
#include "ipsock.hpp"

#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

namespace gopp::net {

const error ErrClosed = errors::New("use of closed network connection");
const error ErrWriteToConnected = errors::New("use of WriteTo with pre-connected connection");
const error errMissingAddress = errors::New("missing address");

namespace {

// Go's canceledError and timeoutError.
struct canceledError final : errorInterface {
    string Error() const override { return string::fromStatic("operation was canceled"); }
    bool Is(const errorInterface* target) const noexcept override { return target == context::Canceled.get(); }
};

struct timeoutError final : errorInterface {
    string Error() const override { return string::fromStatic("i/o timeout"); }
    bool Timeout() const noexcept override { return true; }
    bool Is(const errorInterface* target) const noexcept override {
        return target == context::DeadlineExceeded.get();
    }
};

}  // namespace

const error errCanceled = make_ptr<canceledError>();
const error errTimeout = make_ptr<timeoutError>();

error mapErr(const error& err) {
    if (err == context::Canceled) return errCanceled;
    if (err == context::DeadlineExceeded) return errTimeout;
    return err;
}

error addrSockaddr(const Addr* a, int family, sockaddr_storage* sa, socklen_t* len) {
    *len = 0;
    if (!a) return nullptr;
    if (auto* u = dynamic_cast<const UnixAddr*>(a)) return unixSockaddr(u->Name.view(), sa, len);
    ip_port ip = ipPortOf(a);
    if (!ip.ok) return make_ptr<AddrError>(string::fromStatic("unexpected address type"), a->String());
    return ip_to_sockaddr(family, ip.ip, ip.port, ip.zone.view(), sa, len);
}

netfd* netfdOf(const object* c) {
    if (auto* cb = dynamic_cast<const conn_base*>(c)) return cb->internalFd().get();
    return nullptr;
}

string OpError::Error() const {
    string s = Op;
    if (!Net.empty()) s = s + " " + Net;
    if (Source) s = s + " " + Source->String();
    if (Addr) {
        s = s + (Source ? "->" : " ");
        s = s + Addr->String();
    }
    return s + ": " + Err.Error();
}

gopp::error op_error(std::string_view op, const string& network, ptr<Addr> source, ptr<Addr> addr, gopp::error err) {
    if (!err || err == io::EOF_) return err;
    auto e = make_ptr<OpError>();
    e->Op = string(op);
    e->Net = network;
    e->Source = std::move(source);
    e->Addr = std::move(addr);
    e->Err = std::move(err);
    return e;
}

// ---------------------------------------------------------------------------
// Connections

conn_base::conn_base(ptr<netfd> fd) : fd_(std::move(fd)) {}

conn_base::~conn_base() { fd_->Close(); }

std::pair<isize, error> conn_base::Read(std::span<byte> p) {
    auto [n, err] = fd_->Read(p);
    return {n, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
}

std::pair<isize, error> conn_base::Write(std::span<const byte> p) {
    auto [n, err] = fd_->Write(p);
    return {n, op_error("write", fd_->network(), fd_->laddr, fd_->raddr, err)};
}

error conn_base::Close() { return op_error("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->Close()); }

ptr<net::Addr> conn_base::LocalAddr() const { return fd_->laddr; }
ptr<net::Addr> conn_base::RemoteAddr() const { return fd_->raddr; }

error conn_base::SetDeadline(time::Time t) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->SetDeadline(t, 'r' + 'w'));
}
error conn_base::SetReadDeadline(time::Time t) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->SetDeadline(t, 'r'));
}
error conn_base::SetWriteDeadline(time::Time t) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->SetDeadline(t, 'w'));
}

error conn_base::SetReadBuffer(int bytes) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->setsockoptInt(SOL_SOCKET, SO_RCVBUF, bytes));
}
error conn_base::SetWriteBuffer(int bytes) {
    return op_error("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->setsockoptInt(SOL_SOCKET, SO_SNDBUF, bytes));
}

std::pair<ptr<syscall::RawConn>, error> conn_base::SyscallConn() { return {newRawConn(fd_), nullptr}; }

std::pair<int64, error> conn_base::writeBuffers(Buffers& v) {
    auto [n, err] = fd_->writeBuffers(v);
    return {n, op_error("writev", fd_->network(), fd_->laddr, fd_->raddr, err)};
}

std::pair<ptr<os::File>, error> conn_base::File() {
    auto [f, err] = fd_->dup();
    if (err) return {nullptr, op_error("file", fd_->network(), fd_->laddr, fd_->raddr, err)};
    return {f, nullptr};
}

// ---------------------------------------------------------------------------
// FileConn, FileListener and FilePacketConn (Go's file_unix.go)

namespace {

// Go's fileAddr: the name of the File in errors.
struct fileAddr final : Addr {
    string name;
    explicit fileAddr(string n) : name(std::move(n)) {}
    string Network() const override { return string::fromStatic("file+net"); }
    string String() const override { return name; }
};

// Go's newFileFD: a non-blocking copy of f's socket.
std::pair<ptr<netfd>, error> newFileFd(const ptr<os::File>& f) {
    int s = ::fcntl(static_cast<int>(f->Fd()), F_DUPFD_CLOEXEC, 0);
    if (s < 0) return {nullptr, os::NewSyscallError("dup", errnoError(errno))};
    int fl = ::fcntl(s, F_GETFL);
    if (fl < 0 || ::fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) {
        int e = errno;
        ::close(s);
        return {nullptr, os::NewSyscallError("setnonblock", errnoError(e))};
    }
    int sotype = 0;
    socklen_t optlen = sizeof(sotype);
    sockaddr_storage lsa{};
    socklen_t llen = sizeof(lsa);
    if (::getsockopt(s, SOL_SOCKET, SO_TYPE, &sotype, &optlen) < 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&lsa), &llen) < 0) {
        int e = errno;
        ::close(s);
        return {nullptr, os::NewSyscallError("getsockopt", errnoError(e))};
    }
    int family = lsa.ss_family;
    if (family != AF_INET && family != AF_INET6 && family != AF_UNIX) {
        ::close(s);
        return {nullptr, errnoError(EPROTONOSUPPORT)};
    }
    // The network is the local address's.
    ptr<Addr> la = sockaddrToAddr(lsa, sotype);
    auto fd = make_ptr<netfd>(s, family, sotype, la->Network());
    if (error err = fd->init()) return {nullptr, err};
    fd->setAddrs();
    return {fd, nullptr};
}

template <typename T>
std::pair<ptr<T>, error> fileError(const ptr<os::File>& f, error err) {
    return {nullptr, op_error("file", string::fromStatic("file+net"), nullptr, make_ptr<fileAddr>(f->Name()), err)};
}

}  // namespace

std::pair<ptr<Conn>, error> FileConn(const ptr<os::File>& f) {
    auto [fd, err] = newFileFd(f);
    if (err) return fileError<Conn>(f, err);
    if (dynamic_cast<TCPAddr*>(fd->laddr.get())) return {newTCPConn(fd, 15 * time::Second, KeepAliveConfig{}), nullptr};
    if (dynamic_cast<UDPAddr*>(fd->laddr.get())) return {make_ptr<UDPConn>(fd), nullptr};
    if (dynamic_cast<IPAddr*>(fd->laddr.get())) return {make_ptr<IPConn>(fd), nullptr};
    if (dynamic_cast<UnixAddr*>(fd->laddr.get())) return {make_ptr<UnixConn>(fd), nullptr};
    fd->Close();
    return fileError<Conn>(f, errnoError(EINVAL));
}

std::pair<ptr<Listener>, error> FileListener(const ptr<os::File>& f) {
    auto [fd, err] = newFileFd(f);
    if (err) return fileError<Listener>(f, err);
    if (dynamic_cast<TCPAddr*>(fd->laddr.get())) return {make_ptr<TCPListener>(fd, KeepAliveConfig{}, time::Duration(0)), nullptr};
    // A listener from a File doesn't remove the socket file when closed.
    if (auto* ua = dynamic_cast<UnixAddr*>(fd->laddr.get())) return {make_ptr<UnixListener>(fd, ua->Name, false), nullptr};
    fd->Close();
    return fileError<Listener>(f, errnoError(EINVAL));
}

std::pair<ptr<PacketConn>, error> FilePacketConn(const ptr<os::File>& f) {
    auto [fd, err] = newFileFd(f);
    if (err) return fileError<PacketConn>(f, err);
    if (dynamic_cast<UDPAddr*>(fd->laddr.get())) return {make_ptr<UDPConn>(fd), nullptr};
    if (dynamic_cast<IPAddr*>(fd->laddr.get())) return {make_ptr<IPConn>(fd), nullptr};
    if (dynamic_cast<UnixAddr*>(fd->laddr.get())) return {make_ptr<UnixConn>(fd), nullptr};
    fd->Close();
    return fileError<PacketConn>(f, errnoError(EINVAL));
}

// ---------------------------------------------------------------------------
// Buffers (net.go)

std::pair<int64, error> Buffers::WriteTo(io::Writer& w) {
    if (auto* c = dynamic_cast<conn_base*>(&w)) return c->writeBuffers(*this);
    int64 n = 0;
    for (auto& b : *this) {
        auto [nb, err] = w.Write(b);
        n += nb;
        if (err) {
            consume(n);
            return {n, err};
        }
    }
    consume(n);
    return {n, nullptr};
}

std::pair<isize, error> Buffers::Read(std::span<byte> p) {
    isize n = 0;
    while (!p.empty() && !empty()) {
        std::size_t n0 = Copy(p, std::span<const byte>(front()));
        consume(int64(n0));
        p = p.subspan(n0);
        n += isize(n0);
    }
    if (empty()) return {n, io::EOF_};
    return {n, nullptr};
}

void Buffers::consume(int64 n) {
    std::size_t i = 0;
    while (i < size()) {
        int64 ln0 = int64((*this)[i].size());
        if (ln0 > n) {
            (*this)[i] = (*this)[i].Sub(std::size_t(n));
            break;
        }
        n -= ln0;
        (*this)[i] = slice<byte>();
        i++;
    }
    erase(begin(), begin() + std::ptrdiff_t(i));
}

}  // namespace gopp::net
