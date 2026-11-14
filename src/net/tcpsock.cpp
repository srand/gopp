// TCP, after Go's net/tcpsock.go, tcpsock_posix.go, tcpsock_unix.go,
// tcpsockopt_unix.go, sockopt_posix.go, splice_linux.go, sendfile.go and
// internal/poll's splice_linux.go and sendfile_unix.go.
#include "dial.hpp"
#include "ipsock.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <mutex>
#include <string>

namespace gopp::net {

namespace {

constexpr time::Duration defaultTCPKeepAliveIdle = 15 * time::Second;
constexpr time::Duration defaultTCPKeepAliveInterval = 15 * time::Second;
constexpr int defaultTCPKeepAliveCount = 9;

int64 roundDurationUp(time::Duration d, time::Duration to) { return (d.ns + to.ns - 1) / to.ns; }

error setKeepAliveIdle(netfd& fd, time::Duration d) {
    if (d.ns == 0) d = defaultTCPKeepAliveIdle;
    else if (d.ns < 0) return nullptr;
    return fd.setsockoptInt(IPPROTO_TCP, TCP_KEEPIDLE, int(roundDurationUp(d, time::Second)));
}

error setKeepAliveInterval(netfd& fd, time::Duration d) {
    if (d.ns == 0) d = defaultTCPKeepAliveInterval;
    else if (d.ns < 0) return nullptr;
    return fd.setsockoptInt(IPPROTO_TCP, TCP_KEEPINTVL, int(roundDurationUp(d, time::Second)));
}

error setKeepAliveCount(netfd& fd, int n) {
    if (n == 0) n = defaultTCPKeepAliveCount;
    else if (n < 0) return nullptr;
    return fd.setsockoptInt(IPPROTO_TCP, TCP_KEEPCNT, n);
}

// Go's selfConnect: a socket that connected to itself, which happens
// when the kernel picks the dialed port as the local one.
bool selfConnect(const ptr<netfd>& fd, const error& err) {
    if (err) return false;
    if (!fd->laddr || !fd->raddr) return true;
    ipPort l = ipPortOf(fd->laddr.get()), r = ipPortOf(fd->raddr.get());
    return l.port == r.port && l.ip.Equal(r.ip);
}

bool spuriousENOTAVAIL(const error& err) {
    auto* e = errors::As<Errno>(err);
    return e && e->Code == EADDRNOTAVAIL;
}

// Go's internal/poll splice pipes.
struct splicePipe {
    int rfd = -1, wfd = -1;
    int data = 0;  // bytes in the pipe
};

constexpr int maxSpliceSize = 1 << 20;

std::mutex pipeMu;
std::vector<splicePipe>& pipePool() {
    static auto* v = new std::vector<splicePipe>;
    return *v;
}

bool getPipe(splicePipe* p) {
    {
        std::lock_guard l(pipeMu);
        if (!pipePool().empty()) {
            *p = pipePool().back();
            pipePool().pop_back();
            return true;
        }
    }
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) return false;
    ::fcntl(fds[0], F_SETPIPE_SZ, maxSpliceSize);
    *p = {fds[0], fds[1], 0};
    return true;
}

void putPipe(const splicePipe& p) {
    // A pipe with data left in it can't be reused.
    if (p.data != 0) {
        ::close(p.rfd);
        ::close(p.wfd);
        return;
    }
    std::lock_guard l(pipeMu);
    pipePool().push_back(p);
}

// A reference to a socket's descriptor that keeps Close from releasing it
// (Go's fdMutex references).
struct fdref {
    netfd& fd;
    bool ok;
    explicit fdref(netfd& f) : fd(f), ok(f.incref()) {}
    ~fdref() {
        if (ok) fd.decref();
    }
};

// Go's spliceDrain: from the socket into the pipe.
std::pair<int, error> spliceDrain(int pipefd, netfd& sock, int max) {
    std::lock_guard l(sock.readLock());
    fdref ref(sock);
    if (!ref.ok) return {0, ErrClosed};
    if (error err = sock.prepare('r')) return {0, err};
    for (;;) {
        ssize_t n = ::splice(sock.sysfd(), nullptr, pipefd, nullptr, std::size_t(max), SPLICE_F_NONBLOCK);
        if (n >= 0) return {int(n), nullptr};
        int e = errno;
        if (e == EINTR) continue;
        if (e != EAGAIN) return {0, errnoError(e)};
        if (error err = sock.wait('r')) return {0, err};
    }
}

// Go's splicePump: from the pipe into the socket.
std::pair<int, error> splicePump(netfd& sock, int pipefd, int inPipe) {
    std::lock_guard l(sock.writeLock());
    fdref ref(sock);
    if (!ref.ok) return {0, ErrClosed};
    if (error err = sock.prepare('w')) return {0, err};
    int written = 0;
    while (inPipe > 0) {
        ssize_t n = ::splice(pipefd, nullptr, sock.sysfd(), nullptr, std::size_t(inPipe), SPLICE_F_NONBLOCK);
        if (n > 0) {
            inPipe -= int(n);
            written += int(n);
            continue;
        }
        int e = n < 0 ? errno : EAGAIN;
        if (e == EINTR) continue;
        if (e != EAGAIN) return {written, errnoError(e)};
        if (error err = sock.wait('w')) return {written, err};
    }
    return {written, nullptr};
}

// Go's poll.Splice: up to remain bytes from src to dst through a pipe.
std::tuple<int64, bool, error> pollSplice(netfd& dst, netfd& src, int64 remain) {
    splicePipe p;
    if (!getPipe(&p)) return {0, false, errnoError(EINVAL)};
    int64 written = 0;
    bool handled = false;
    error err;
    while (!err && remain > 0) {
        int max = int(std::min<int64>(maxSpliceSize, remain));
        int inPipe;
        std::tie(inPipe, err) = spliceDrain(p.wfd, src, max);
        // EINVAL: splice can't do these descriptors; let the caller copy.
        auto* e = errors::As<Errno>(err);
        handled = handled || !(e && e->Code == EINVAL);
        if (err || inPipe == 0) break;
        p.data += inPipe;
        int n;
        std::tie(n, err) = splicePump(dst, p.rfd, inPipe);
        if (n > 0) {
            written += n;
            remain -= n;
            p.data -= n;
        }
    }
    putPipe(p);
    if (err) return {written, handled, err};
    return {written, true, nullptr};
}

error wrapSyscallError(std::string_view name, const error& err) {
    if (errors::As<Errno>(err) && !errors::As<os::SyscallError>(err)) return os::NewSyscallError(string(name), err);
    return err;
}

// Go's spliceFrom: r is a TCP or Unix stream connection, maybe in a
// LimitedReader.
std::tuple<int64, error, bool> spliceFrom(netfd& c, io::Reader& r0) {
    int64 remain = std::numeric_limits<int64>::max();
    io::Reader* r = &r0;
    auto* lr = dynamic_cast<io::LimitedReader*>(r);
    if (lr) {
        remain = lr->N;
        r = lr->R.get();
        if (remain <= 0) return {0, nullptr, true};
    }
    netfd* s = nullptr;
    if (auto* t = dynamic_cast<TCPConn*>(r)) s = t->internalFd().get();
    else if (auto* u = dynamic_cast<UnixConn*>(r); u && u->internalFd()->network() == "unix") s = u->internalFd().get();
    if (!s) return {0, nullptr, false};
    auto [written, handled, err] = pollSplice(c, *s, remain);
    if (lr) lr->N -= written;
    return {written, wrapSyscallError("splice", err), handled};
}

// Go's sendFile: r is a regular os::File, maybe in a LimitedReader;
// sendfile(2) sends from its offset.
std::tuple<int64, error, bool> sendFile(netfd& c, io::Reader& r0) {
    int64 remain = 0;  // 0: the whole file
    io::Reader* r = &r0;
    auto* lr = dynamic_cast<io::LimitedReader*>(r);
    if (lr) {
        remain = lr->N;
        r = lr->R.get();
        if (remain <= 0) return {0, nullptr, true};
    }
    auto* f = dynamic_cast<os::File*>(r);
    if (!f) return {0, nullptr, false};
    // Only regular files: Fd() would make a pipe's descriptor blocking.
    auto [fi, serr] = f->Stat();
    if (serr || !fi->Mode().IsRegular()) return {0, nullptr, false};
    int src = int(f->Fd());

    std::lock_guard l(c.writeLock());
    fdref ref(c);
    if (!ref.ok) return {0, ErrClosed, true};
    if (error err = c.prepare('w')) return {0, err, true};
    int64 written = 0;
    for (;;) {
        std::size_t chunk = (std::size_t(1) << 31) - 1;
        if (remain > 0) chunk = std::size_t(std::min<int64>(remain - written, int64(chunk)));
        ssize_t n = ::sendfile(c.sysfd(), src, nullptr, chunk);
        int e = n < 0 ? errno : 0;
        if (n > 0) written += n;
        if (e == 0) {
            if (n == 0 || (remain > 0 && written >= remain)) break;
            continue;
        }
        if (e == EAGAIN) {
            if (remain > 0 && written >= remain) break;
            if (error err = c.wait('w')) {
                if (lr) lr->N = remain - written;
                return {written, err, true};
            }
            continue;
        }
        if (e == EINTR) continue;
        if (lr) lr->N = remain - written;
        bool handled = !(e == ENOSYS || e == EOPNOTSUPP || e == EINVAL) || written > 0;
        return {written, wrapSyscallError("sendfile", errnoError(e)), handled};
    }
    if (lr) lr->N = remain - written;
    return {written, nullptr, true};
}

// io::Copy's loop, without the ReaderFrom and WriterTo calls that would
// lead back here (Go's genericReadFrom and genericWriteTo).
struct writerOnly final : io::Writer {
    io::Writer& w;
    explicit writerOnly(io::Writer& w_) : w(w_) {}
    std::pair<isize, error> Write(std::span<const byte> p) override { return w.Write(p); }
};

struct readerOnly final : io::Reader {
    io::Reader& r;
    explicit readerOnly(io::Reader& r_) : r(r_) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return r.Read(p); }
};

}  // namespace

netip::AddrPort TCPAddr::AddrPort() const {
    auto [na, ok] = netip::AddrFromSlice(IP);
    na = na.WithZone(Zone.view());
    return netip::AddrPortFrom(na, uint16(Port));
}

ptr<TCPAddr> TCPAddrFromAddrPort(netip::AddrPort addr) {
    return make_ptr<TCPAddr>(net::IP(addr.Addr().AsSlice()), int(addr.Port()), addr.Addr().Zone());
}

ptr<TCPConn> newTCPConn(ptr<netfd> fd, time::Duration keepAliveIdle, KeepAliveConfig keepAliveCfg) {
    fd->setsockoptInt(IPPROTO_TCP, TCP_NODELAY, 1);
    if (!keepAliveCfg.Enable && keepAliveIdle.ns >= 0) keepAliveCfg = KeepAliveConfig{.Enable = true, .Idle = keepAliveIdle};
    auto c = make_ptr<TCPConn>(std::move(fd));
    if (keepAliveCfg.Enable) c->SetKeepAliveConfig(keepAliveCfg);
    return c;
}

std::pair<int64, error> TCPConn::ReadFrom(io::Reader& r) {
    auto [n, err, handled] = spliceFrom(*fd_, r);
    if (!handled) std::tie(n, err, handled) = sendFile(*fd_, r);
    if (!handled) {
        writerOnly w(*this);
        std::tie(n, err) = io::Copy(w, r);
    }
    if (err && err != io::EOF_) err = opError("readfrom", fd_->network(), fd_->laddr, fd_->raddr, err);
    return {n, err};
}

std::pair<int64, error> TCPConn::WriteTo(io::Writer& w) {
    int64 n = 0;
    error err;
    bool handled = false;
    // Go's spliceTo: to a Unix stream connection.
    if (auto* uc = dynamic_cast<UnixConn*>(&w); uc && uc->internalFd()->network() == "unix") {
        std::tie(n, handled, err) = pollSplice(*uc->internalFd(), *fd_, std::numeric_limits<int64>::max());
        err = wrapSyscallError("splice", err);
    }
    if (!handled) {
        readerOnly r(*this);
        writerOnly wo(w);
        std::tie(n, err) = io::Copy(wo, r);
    }
    if (err && err != io::EOF_) err = opError("writeto", fd_->network(), fd_->laddr, fd_->raddr, err);
    return {n, err};
}

error TCPConn::CloseRead() {
    return opError("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_RD));
}

error TCPConn::CloseWrite() {
    return opError("close", fd_->network(), fd_->laddr, fd_->raddr, fd_->shutdown(SHUT_WR));
}

error TCPConn::SetLinger(int sec) {
    return opError("set", fd_->network(), fd_->laddr, fd_->raddr, fd_->setsockoptLinger(sec));
}

error TCPConn::SetKeepAlive(bool keepalive) {
    return opError("set", fd_->network(), fd_->laddr, fd_->raddr,
                    fd_->setsockoptInt(SOL_SOCKET, SO_KEEPALIVE, keepalive));
}

error TCPConn::SetKeepAlivePeriod(time::Duration d) {
    return opError("set", fd_->network(), fd_->laddr, fd_->raddr, setKeepAliveIdle(*fd_, d));
}

error TCPConn::SetKeepAliveConfig(const KeepAliveConfig& config) {
    auto fail = [&](error err) { return opError("set", fd_->network(), fd_->laddr, fd_->raddr, err); };
    if (error err = fd_->setsockoptInt(SOL_SOCKET, SO_KEEPALIVE, config.Enable)) return fail(err);
    if (error err = setKeepAliveIdle(*fd_, config.Idle)) return fail(err);
    if (error err = setKeepAliveInterval(*fd_, config.Interval)) return fail(err);
    if (error err = setKeepAliveCount(*fd_, config.Count)) return fail(err);
    return nullptr;
}

error TCPConn::SetNoDelay(bool noDelay) {
    return opError("set", fd_->network(), fd_->laddr, fd_->raddr,
                    fd_->setsockoptInt(IPPROTO_TCP, TCP_NODELAY, noDelay));
}

std::pair<bool, error> TCPConn::MultipathTCP() { return {isUsingMultipathTCP(*fd_), nullptr}; }

// ---------------------------------------------------------------------------
// TCPListener

TCPListener::TCPListener(ptr<netfd> fd, KeepAliveConfig keepalive, time::Duration keepAliveIdle)
    : fd_(std::move(fd)), keepalive_(keepalive), keepAliveIdle_(keepAliveIdle) {}

TCPListener::~TCPListener() { fd_->Close(); }

std::pair<ptr<TCPConn>, error> TCPListener::AcceptTCP() {
    auto [nfd, err] = fd_->accept();
    if (err) return {nullptr, opError("accept", fd_->network(), nullptr, fd_->laddr, err)};
    return {newTCPConn(nfd, keepAliveIdle_, keepalive_), nullptr};
}

std::pair<ptr<Conn>, error> TCPListener::Accept() {
    auto [c, err] = AcceptTCP();
    return {c, err};
}

error TCPListener::Close() {
    if (error err = fd_->Close()) return opError("close", fd_->network(), nullptr, fd_->laddr, err);
    return nullptr;
}

ptr<net::Addr> TCPListener::Addr() const { return fd_->laddr; }

error TCPListener::SetDeadline(time::Time t) { return fd_->SetDeadline(t, 'r' + 'w'); }

std::pair<ptr<os::File>, error> TCPListener::File() {
    auto [f, err] = fd_->dup();
    if (err) return {nullptr, opError("file", fd_->network(), nullptr, fd_->laddr, err)};
    return {f, nullptr};
}

std::pair<ptr<syscall::RawConn>, error> TCPListener::SyscallConn() { return {newRawConn(fd_, true), nullptr}; }

// ---------------------------------------------------------------------------
// Dialing and listening

std::pair<ptr<TCPConn>, error> sysDialer::dialTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr,
                                                  const TCPAddr* raddr, int proto) {
    auto ctrl = ctrlCtxFn();
    auto [fd, err] = internetSocket(ctx, network.view(), laddr, raddr, SOCK_STREAM, proto, "dial", ctrl);

    // TCP has a rarely used mechanism called a 'simultaneous connection'
    // in which a socket connects to itself when it picks the dialed port
    // as its local one. Dial again a couple of times if that happens, and
    // also on a spurious EADDRNOTAVAIL.
    for (int i = 0; i < 2 && (!laddr || laddr->Port == 0) && (selfConnect(fd, err) || spuriousENOTAVAIL(err)); i++) {
        if (!err) fd->Close();
        std::tie(fd, err) = internetSocket(ctx, network.view(), laddr, raddr, SOCK_STREAM, proto, "dial", ctrl);
    }
    if (err) return {nullptr, err};
    return {newTCPConn(fd, d.KeepAlive, d.KeepAliveConfig), nullptr};
}

std::pair<ptr<TCPListener>, error> sysListener::listenTCP(const ptr<context::Context>& ctx, const TCPAddr* laddr,
                                                          int proto) {
    auto [fd, err] = internetSocket(ctx, network.view(), laddr, nullptr, SOCK_STREAM, proto, "listen", ctrlCtxFn());
    if (err) return {nullptr, err};
    return {make_ptr<TCPListener>(fd, lc.KeepAliveConfig, lc.KeepAlive), nullptr};
}

std::pair<ptr<TCPConn>, error> DialTCP(std::string_view network, const ptr<TCPAddr>& laddr, const ptr<TCPAddr>& raddr) {
    string net(network);
    if (network != "tcp" && network != "tcp4" && network != "tcp6") {
        return {nullptr, opError("dial", net, laddr, raddr, make_ptr<UnknownNetworkError>(net))};
    }
    if (!raddr) return {nullptr, opError("dial", net, laddr, nullptr, errMissingAddress)};
    sysDialer sd{.d = {}, .network = net, .address = raddr->String()};
    auto ctx = context::Background();
    auto [c, err] = sd.d.MultipathTCP() ? sd.dialMPTCP(ctx, laddr.get(), raddr.get())
                                        : sd.dialTCP(ctx, laddr.get(), raddr.get());
    if (err) return {nullptr, opError("dial", net, laddr, raddr, err)};
    return {c, nullptr};
}

std::pair<ptr<TCPListener>, error> ListenTCP(std::string_view network, const ptr<TCPAddr>& laddr0) {
    string net(network);
    if (network != "tcp" && network != "tcp4" && network != "tcp6") {
        return {nullptr, opError("listen", net, nullptr, laddr0, make_ptr<UnknownNetworkError>(net))};
    }
    ptr<TCPAddr> laddr = laddr0 ? laddr0 : make_ptr<TCPAddr>();
    sysListener sl{.lc = {}, .network = net, .address = laddr->String()};
    auto ctx = context::Background();
    auto [ln, err] = sl.lc.MultipathTCP() ? sl.listenMPTCP(ctx, laddr.get()) : sl.listenTCP(ctx, laddr.get());
    if (err) return {nullptr, opError("listen", net, nullptr, laddr0, err)};
    return {ln, nullptr};
}

}  // namespace gopp::net
