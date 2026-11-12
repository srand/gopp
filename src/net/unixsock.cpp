// Unix domain sockets, after Go's net/unixsock.go and unixsock_posix.go.
#include "fd.hpp"
#include "unixsock.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

namespace gopp::net {

namespace {

int unix_sotype(std::string_view net) {
    if (net == "unix") return SOCK_STREAM;
    if (net == "unixgram") return SOCK_DGRAM;
    if (net == "unixpacket") return SOCK_SEQPACKET;
    return -1;
}

ptr<UnixAddr> as_unix(const ptr<Addr>& a) { return ptr<UnixAddr>(dynamic_cast<UnixAddr*>(a.get())); }

}  // namespace

std::pair<ptr<UnixAddr>, error> ResolveUnixAddr(std::string_view network, std::string_view address) {
    if (unix_sotype(network) < 0) return {nullptr, make_ptr<UnknownNetworkError>(string(network))};
    return {make_ptr<UnixAddr>(string(address), string(network)), nullptr};
}

std::pair<ptr<netfd>, error> unix_socket(std::string_view net, const UnixAddr* laddr, const UnixAddr* raddr,
                                         bool listen) {
    int sotype = unix_sotype(net);
    if (sotype < 0) return {nullptr, make_ptr<UnknownNetworkError>(string(net))};
    if (!listen) {
        // Unnamed addresses don't count when dialing.
        if (laddr && laddr->Name.empty()) laddr = nullptr;
        if (raddr && raddr->Name.empty()) raddr = nullptr;
        if (!raddr && (sotype != SOCK_DGRAM || !laddr)) return {nullptr, errMissingAddress};
    }
    int s = ::socket(AF_UNIX, sotype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return {nullptr, os::NewSyscallError("socket", errno_error(errno))};
    auto fd = make_ptr<netfd>(s, AF_UNIX, sotype, string(net));
    if (laddr) {
        sockaddr_storage sa;
        socklen_t len;
        if (error err = unix_sockaddr(laddr->Name.view(), &sa, &len)) return {nullptr, err};
        if (::bind(s, reinterpret_cast<sockaddr*>(&sa), len) < 0) {
            return {nullptr, os::NewSyscallError("bind", errno_error(errno))};
        }
    }
    if (listen && sotype != SOCK_DGRAM && ::listen(s, SOMAXCONN) < 0) {
        return {nullptr, os::NewSyscallError("listen", errno_error(errno))};
    }
    if (error err = fd->init()) return {nullptr, err};
    if (raddr) {
        sockaddr_storage sa;
        socklen_t len;
        if (error err = unix_sockaddr(raddr->Name.view(), &sa, &len)) return {nullptr, err};
        if (error err = fd->connect(&sa, len, 0)) {
            if (errors::As<Errno>(err)) err = os::NewSyscallError("connect", err);
            return {nullptr, err};
        }
    }
    fd->set_addrs();
    return {fd, nullptr};
}

// ---------------------------------------------------------------------------
// UnixConn

error UnixConn::CloseRead() {
    return op_error("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_RD));
}

error UnixConn::CloseWrite() {
    return op_error("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_WR));
}

std::tuple<isize, ptr<UnixAddr>, error> UnixConn::ReadFromUnix(std::span<byte> b) {
    auto [n, oobn, flags, addr, err] = ReadMsgUnix(b, {});
    return {n, addr, err};
}

std::tuple<isize, ptr<net::Addr>, error> UnixConn::ReadFrom(std::span<byte> b) {
    auto [n, addr, err] = ReadFromUnix(b);
    return {n, addr, err};
}

std::tuple<isize, isize, int, ptr<UnixAddr>, error> UnixConn::ReadMsgUnix(std::span<byte> b, std::span<byte> oob) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, oob, 0, &from);
    // An unnamed sender has no address.
    ptr<UnixAddr> addr = as_unix(sockaddr_to_addr(from, fd_->sotype()));
    if (addr && addr->Name.empty()) addr = nullptr;
    return {n, oobn, flags, addr, op_error("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
}

std::pair<isize, error> UnixConn::WriteToUnix(std::span<const byte> b, const ptr<UnixAddr>& addr) {
    auto fail = [&](error err) {
        return std::pair<isize, error>{0, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->raddr) return fail(ErrWriteToConnected);
    if (!addr) return fail(errMissingAddress);
    if (addr->Net != unix_net(fd_->sotype())) return fail(errno_error(EAFNOSUPPORT));
    sockaddr_storage sa;
    socklen_t len;
    if (error err = unix_sockaddr(addr->Name.view(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, {}, &sa, len);
    if (err) return {n, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, nullptr};
}

std::pair<isize, error> UnixConn::WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) {
    auto a = as_unix(addr);
    if (!a) return {0, op_error("write", fd_->network(), fd_->laddr, addr, errno_error(EINVAL))};
    return WriteToUnix(b, a);
}

std::tuple<isize, isize, error> UnixConn::WriteMsgUnix(std::span<const byte> b, std::span<const byte> oob,
                                                       const ptr<UnixAddr>& addr) {
    auto fail = [&](error err) {
        return std::tuple<isize, isize, error>{0, 0, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->sotype() == SOCK_DGRAM && fd_->raddr && addr) return fail(ErrWriteToConnected);
    sockaddr_storage sa;
    socklen_t len = 0;
    if (addr) {
        if (addr->Net != unix_net(fd_->sotype())) return fail(errno_error(EAFNOSUPPORT));
        if (error err = unix_sockaddr(addr->Name.view(), &sa, &len)) return fail(err);
    }
    auto [n, oobn, err] = fd_->writeMsg(b, oob, addr ? &sa : nullptr, len);
    if (err) return {n, oobn, op_error("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, oobn, nullptr};
}

// ---------------------------------------------------------------------------
// UnixListener

UnixListener::UnixListener(ptr<netfd> fd, string path, bool unlink)
    : fd_(std::move(fd)), path_(std::move(path)), unlink_(unlink) {}

UnixListener::~UnixListener() { Close(); }

std::pair<ptr<UnixConn>, error> UnixListener::AcceptUnix() {
    auto [nfd, err] = fd_->accept();
    if (err) return {nullptr, op_error("accept", fd_->network(), nullptr, fd_->laddr, err)};
    return {make_ptr<UnixConn>(nfd), nullptr};
}

std::pair<ptr<Conn>, error> UnixListener::Accept() {
    auto [c, err] = AcceptUnix();
    return {c, err};
}

error UnixListener::Close() {
    // Only the first Close removes the file, which may be another's by the
    // second.
    if (!unlinked_.exchange(true) && unlink_ && !path_.empty() && path_.view()[0] != '@') {
        std::string p(path_.view());
        ::unlink(p.c_str());
    }
    if (error err = fd_->Close()) return op_error("close", fd_->network(), nullptr, fd_->laddr, err);
    return nullptr;
}

ptr<net::Addr> UnixListener::Addr() const { return fd_->laddr; }

error UnixListener::SetDeadline(time::Time t) { return fd_->SetDeadline(t, 'r' + 'w'); }

std::pair<ptr<os::File>, error> UnixListener::File() {
    auto [f, err] = fd_->dup();
    if (err) return {nullptr, op_error("file", fd_->network(), nullptr, fd_->laddr, err)};
    return {f, nullptr};
}

std::pair<ptr<UnixListener>, error> listen_unix(std::string_view net, const UnixAddr* laddr) {
    auto [fd, err] = unix_socket(net, laddr, nullptr, true);
    if (err) return {nullptr, err};
    string path = fd->laddr ? fd->laddr->String() : string();
    return {make_ptr<UnixListener>(fd, path, true), nullptr};
}

std::pair<ptr<UnixListener>, error> ListenUnix(std::string_view network, const ptr<UnixAddr>& laddr) {
    string net(network);
    if (network != "unix" && network != "unixpacket") {
        return {nullptr, op_error("listen", net, nullptr, laddr, make_ptr<UnknownNetworkError>(net))};
    }
    if (!laddr) return {nullptr, op_error("listen", net, nullptr, nullptr, errMissingAddress)};
    auto [ln, err] = listen_unix(network, laddr.get());
    if (err) return {nullptr, op_error("listen", net, nullptr, laddr, err)};
    return {ln, nullptr};
}

std::pair<ptr<UnixConn>, error> ListenUnixgram(std::string_view network, const ptr<UnixAddr>& laddr) {
    string net(network);
    if (network != "unixgram") return {nullptr, op_error("listen", net, nullptr, laddr, make_ptr<UnknownNetworkError>(net))};
    if (!laddr) return {nullptr, op_error("listen", net, nullptr, nullptr, errMissingAddress)};
    auto [fd, err] = unix_socket(network, laddr.get(), nullptr, true);
    if (err) return {nullptr, op_error("listen", net, nullptr, laddr, err)};
    return {make_ptr<UnixConn>(fd), nullptr};
}

std::pair<ptr<UnixConn>, error> DialUnix(std::string_view network, const ptr<UnixAddr>& laddr,
                                         const ptr<UnixAddr>& raddr) {
    string net(network);
    if (unix_sotype(network) < 0) return {nullptr, op_error("dial", net, laddr, raddr, make_ptr<UnknownNetworkError>(net))};
    auto [fd, err] = unix_socket(network, laddr.get(), raddr.get(), false);
    if (err) return {nullptr, op_error("dial", net, laddr, raddr, err)};
    return {make_ptr<UnixConn>(fd), nullptr};
}

}  // namespace gopp::net
