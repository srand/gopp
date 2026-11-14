// Unix domain sockets, after Go's net/unixsock.go and unixsock_posix.go.
#include "dial.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

namespace gopp::net {

namespace {

int unixSotype(std::string_view net) {
    if (net == "unix") return SOCK_STREAM;
    if (net == "unixgram") return SOCK_DGRAM;
    if (net == "unixpacket") return SOCK_SEQPACKET;
    return -1;
}

ptr<UnixAddr> asUnix(const ptr<Addr>& a) { return ptr<UnixAddr>(dynamic_cast<UnixAddr*>(a.get())); }

}  // namespace

std::pair<ptr<UnixAddr>, error> ResolveUnixAddr(std::string_view network, std::string_view address) {
    if (unixSotype(network) < 0) return {nullptr, make_ptr<UnknownNetworkError>(string(network))};
    return {make_ptr<UnixAddr>(string(address), string(network)), nullptr};
}

// ---------------------------------------------------------------------------
// UnixConn

error UnixConn::CloseRead() {
    return opError("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_RD));
}

error UnixConn::CloseWrite() {
    return opError("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_WR));
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
    ptr<UnixAddr> addr = asUnix(sockaddrToAddr(from, fd_->sotype()));
    if (addr && addr->Name.empty()) addr = nullptr;
    return {n, oobn, flags, addr, opError("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
}

std::pair<isize, error> UnixConn::WriteToUnix(std::span<const byte> b, const ptr<UnixAddr>& addr) {
    auto fail = [&](error err) {
        return std::pair<isize, error>{0, opError("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->raddr) return fail(ErrWriteToConnected);
    if (!addr) return fail(errMissingAddress);
    if (addr->Net != unixNet(fd_->sotype())) return fail(errnoError(EAFNOSUPPORT));
    sockaddr_storage sa;
    socklen_t len;
    if (error err = unixSockaddr(addr->Name.view(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, {}, &sa, len);
    if (err) return {n, opError("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, nullptr};
}

std::pair<isize, error> UnixConn::WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) {
    auto a = asUnix(addr);
    if (!a) return {0, opError("write", fd_->network(), fd_->laddr, addr, errnoError(EINVAL))};
    return WriteToUnix(b, a);
}

std::tuple<isize, isize, error> UnixConn::WriteMsgUnix(std::span<const byte> b, std::span<const byte> oob,
                                                       const ptr<UnixAddr>& addr) {
    auto fail = [&](error err) {
        return std::tuple<isize, isize, error>{0, 0, opError("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->sotype() == SOCK_DGRAM && fd_->raddr && addr) return fail(ErrWriteToConnected);
    sockaddr_storage sa;
    socklen_t len = 0;
    if (addr) {
        if (addr->Net != unixNet(fd_->sotype())) return fail(errnoError(EAFNOSUPPORT));
        if (error err = unixSockaddr(addr->Name.view(), &sa, &len)) return fail(err);
    }
    auto [n, oobn, err] = fd_->writeMsg(b, oob, addr ? &sa : nullptr, len);
    if (err) return {n, oobn, opError("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, oobn, nullptr};
}

// ---------------------------------------------------------------------------
// UnixListener

UnixListener::UnixListener(ptr<netfd> fd, string path, bool unlink)
    : fd_(std::move(fd)), path_(std::move(path)), unlink_(unlink) {}

UnixListener::~UnixListener() { Close(); }

std::pair<ptr<UnixConn>, error> UnixListener::AcceptUnix() {
    auto [nfd, err] = fd_->accept();
    if (err) return {nullptr, opError("accept", fd_->network(), nullptr, fd_->laddr, err)};
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
    if (error err = fd_->Close()) return opError("close", fd_->network(), nullptr, fd_->laddr, err);
    return nullptr;
}

ptr<net::Addr> UnixListener::Addr() const { return fd_->laddr; }

error UnixListener::SetDeadline(time::Time t) { return fd_->SetDeadline(t, 'r' + 'w'); }

std::pair<ptr<os::File>, error> UnixListener::File() {
    auto [f, err] = fd_->dup();
    if (err) return {nullptr, opError("file", fd_->network(), nullptr, fd_->laddr, err)};
    return {f, nullptr};
}

std::pair<ptr<syscall::RawConn>, error> UnixListener::SyscallConn() { return {newRawConn(fd_, true), nullptr}; }

// ---------------------------------------------------------------------------
// Dialing and listening

std::pair<ptr<UnixConn>, error> sysDialer::dialUnix(const ptr<context::Context>& ctx, const UnixAddr* laddr,
                                                    const UnixAddr* raddr) {
    auto [fd, err] = unixSocket(ctx, network.view(), laddr, raddr, "dial", ctrlCtxFn());
    if (err) return {nullptr, err};
    return {make_ptr<UnixConn>(fd), nullptr};
}

std::pair<ptr<UnixListener>, error> sysListener::listenUnix(const ptr<context::Context>& ctx, const UnixAddr* laddr) {
    auto [fd, err] = unixSocket(ctx, network.view(), laddr, nullptr, "listen", ctrlCtxFn());
    if (err) return {nullptr, err};
    string path = fd->laddr ? fd->laddr->String() : string();
    return {make_ptr<UnixListener>(fd, path, true), nullptr};
}

std::pair<ptr<UnixConn>, error> sysListener::listenUnixgram(const ptr<context::Context>& ctx, const UnixAddr* laddr) {
    auto [fd, err] = unixSocket(ctx, network.view(), laddr, nullptr, "listen", ctrlCtxFn());
    if (err) return {nullptr, err};
    return {make_ptr<UnixConn>(fd), nullptr};
}

std::pair<ptr<UnixListener>, error> ListenUnix(std::string_view network, const ptr<UnixAddr>& laddr) {
    string net(network);
    if (network != "unix" && network != "unixpacket") {
        return {nullptr, opError("listen", net, nullptr, laddr, make_ptr<UnknownNetworkError>(net))};
    }
    if (!laddr) return {nullptr, opError("listen", net, nullptr, nullptr, errMissingAddress)};
    sysListener sl{.lc = {}, .network = net, .address = laddr->String()};
    auto [ln, err] = sl.listenUnix(context::Background(), laddr.get());
    if (err) return {nullptr, opError("listen", net, nullptr, laddr, err)};
    return {ln, nullptr};
}

std::pair<ptr<UnixConn>, error> ListenUnixgram(std::string_view network, const ptr<UnixAddr>& laddr) {
    string net(network);
    if (network != "unixgram") return {nullptr, opError("listen", net, nullptr, laddr, make_ptr<UnknownNetworkError>(net))};
    if (!laddr) return {nullptr, opError("listen", net, nullptr, nullptr, errMissingAddress)};
    sysListener sl{.lc = {}, .network = net, .address = laddr->String()};
    auto [c, err] = sl.listenUnixgram(context::Background(), laddr.get());
    if (err) return {nullptr, opError("listen", net, nullptr, laddr, err)};
    return {c, nullptr};
}

std::pair<ptr<UnixConn>, error> DialUnix(std::string_view network, const ptr<UnixAddr>& laddr,
                                         const ptr<UnixAddr>& raddr) {
    string net(network);
    if (unixSotype(network) < 0) return {nullptr, opError("dial", net, laddr, raddr, make_ptr<UnknownNetworkError>(net))};
    sysDialer sd{.d = {}, .network = net, .address = raddr ? raddr->String() : string()};
    auto [c, err] = sd.dialUnix(context::Background(), laddr.get(), raddr.get());
    if (err) return {nullptr, opError("dial", net, laddr, raddr, err)};
    return {c, nullptr};
}

}  // namespace gopp::net
