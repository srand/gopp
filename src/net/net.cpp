#include "fd.hpp"

#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gopp::net {

const error ErrClosed = errors::New("use of closed network connection");

string OpError::Error() const {
    std::string s(Op.view());
    if (!Net.empty()) {
        s += ' ';
        s += Net.view();
    }
    if (!Source.empty()) {
        s += ' ';
        s += Source.view();
    }
    if (!Addr.empty()) {
        s += Source.empty() ? " " : "->";
        s += Addr.view();
    }
    s += ": ";
    s += Err.Error().view();
    return string(s);
}


namespace {

gopp::error op_error(std::string_view op, const string& network, const string& source, const string& addr, gopp::error err) {
    if (!err || err == io::EOF_) return err;
    auto e = make_ptr<OpError>();
    e->Op = string(op);
    e->Net = network;
    e->Source = source;
    e->Addr = addr;
    e->Err = std::move(err);
    return e;
}

int family_for(std::string_view network) {
    if (network == "tcp4") return AF_INET;
    if (network == "tcp6") return AF_INET6;
    return AF_UNSPEC;
}

// Resolves host:port to sockaddrs. Names other than numeric addresses and
// "localhost" go through getaddrinfo as a blocking call.
struct resolved {
    sockaddr_storage sa;
    socklen_t len;
};

std::pair<std::vector<resolved>, gopp::error> resolve(std::string_view network, std::string_view address, bool passive) {
    auto [host, port, err] = SplitHostPort(address);
    if (err) return {{}, err};
    int family = family_for(network);
    std::vector<resolved> out;

    // Port: a number or a service name.
    int portnum = -1;
    if (auto [p, perr] = strconv::Atoi(port.view()); !perr && p >= 0 && p <= 65535) {
        portnum = static_cast<int>(p);
    }

    auto add4 = [&](const in_addr& a, int p) {
        resolved r{};
        auto* in = reinterpret_cast<sockaddr_in*>(&r.sa);
        in->sin_family = AF_INET;
        in->sin_port = htons(static_cast<uint16_t>(p));
        in->sin_addr = a;
        r.len = sizeof(sockaddr_in);
        out.push_back(r);
    };
    auto add6 = [&](const in6_addr& a, int p) {
        resolved r{};
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&r.sa);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(static_cast<uint16_t>(p));
        in6->sin6_addr = a;
        r.len = sizeof(sockaddr_in6);
        out.push_back(r);
    };

    if (portnum >= 0) {
        std::string h(host.view());
        in_addr a4;
        in6_addr a6;
        if (h.empty()) {
            if (passive) {
                // Listen on all addresses: IPv6 dual-stack when possible.
                if (family != AF_INET) add6(in6addr_any, portnum);
                if (family != AF_INET6) add4(in_addr{htonl(INADDR_ANY)}, portnum);
            } else {
                if (family != AF_INET6) add4(in_addr{htonl(INADDR_LOOPBACK)}, portnum);
                if (family != AF_INET) add6(in6addr_loopback, portnum);
            }
            return {out, nullptr};
        }
        if (inet_pton(AF_INET, h.c_str(), &a4) == 1 && family != AF_INET6) {
            add4(a4, portnum);
            return {out, nullptr};
        }
        if (inet_pton(AF_INET6, h.c_str(), &a6) == 1 && family != AF_INET) {
            add6(a6, portnum);
            return {out, nullptr};
        }
        if (h == "localhost") {
            if (family != AF_INET6) add4(in_addr{htonl(INADDR_LOOPBACK)}, portnum);
            if (family != AF_INET) add6(in6addr_loopback, portnum);
            return {out, nullptr};
        }
    }

    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    addrinfo* res = nullptr;
    std::string h(host.view()), p(port.view());
    int rc = runtime::blocking([&] { return getaddrinfo(h.empty() ? nullptr : h.c_str(), p.c_str(), &hints, &res); });
    if (rc != 0) return {{}, errors::New(string("lookup ") + host + ": " + gai_strerror(rc))};
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        resolved r{};
        std::memcpy(&r.sa, ai->ai_addr, ai->ai_addrlen);
        r.len = ai->ai_addrlen;
        out.push_back(r);
    }
    freeaddrinfo(res);
    return {out, nullptr};
}

int listen_backlog() {
    static int backlog = [] {
        int n = SOMAXCONN;
        if (FILE* f = std::fopen("/proc/sys/net/core/somaxconn", "r")) {
            int v;
            if (std::fscanf(f, "%d", &v) == 1 && v > 0) n = v > 65535 ? 65535 : v;
            std::fclose(f);
        }
        return n;
    }();
    return backlog;
}

class TCPListener final : public Listener {
public:
    explicit TCPListener(ptr<netfd> fd) : fd_(std::move(fd)) {}
    ~TCPListener() override { fd_->Close(); }

    std::pair<ptr<Conn>, error> Accept() override {
        auto [nfd, err] = fd_->accept();
        if (err) return {nullptr, op_error("accept", fd_->network(), {}, fd_->LocalAddr, err)};
        int one = 1;
        setsockopt(nfd->sysfd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return {make_ptr<TCPConn>(nfd), nullptr};
    }

    error Close() override {
        if (error err = fd_->Close()) return op_error("close", fd_->network(), {}, fd_->LocalAddr, err);
        return nullptr;
    }

    string Addr() const override { return fd_->LocalAddr; }

private:
    ptr<netfd> fd_;
};

}  // namespace

std::tuple<string, string, error> SplitHostPort(std::string_view hp) {
    auto missing_port = [&] { return std::tuple<string, string, error>{{}, {}, errors::New(string("address ") + string(hp) + ": missing port in address")}; };
    auto i = hp.rfind(':');
    if (i == hp.npos) return missing_port();
    std::string_view host, port = hp.substr(i + 1);
    if (!hp.empty() && hp[0] == '[') {
        auto end = hp.find(']');
        if (end == hp.npos) return {{}, {}, errors::New(string("address ") + string(hp) + ": missing ']' in address")};
        if (end + 1 != i) return missing_port();
        host = hp.substr(1, end - 1);
    } else {
        host = hp.substr(0, i);
        if (host.find(':') != host.npos) return {{}, {}, errors::New(string("address ") + string(hp) + ": too many colons in address")};
    }
    return {string(host), string(port), nullptr};
}

string JoinHostPort(std::string_view host, std::string_view port) {
    if (host.find(':') != host.npos) return string("[") + string(host) + "]:" + string(port);
    return string(host) + ":" + string(port);
}

std::pair<ptr<Listener>, error> Listen(std::string_view network, std::string_view address) {
    string net(network);
    if (network != "tcp" && network != "tcp4" && network != "tcp6") {
        return {nullptr, op_error("listen", net, {}, string(address), errors::New("unknown network " + net))};
    }
    auto [addrs, rerr] = resolve(network, address, true);
    if (rerr) return {nullptr, op_error("listen", net, {}, string(address), rerr)};

    error last;
    for (auto& a : addrs) {
        int s = ::socket(a.sa.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (s < 0) {
            last = errno_error(errno);
            continue;
        }
        int one = 1, zero = 0;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (a.sa.ss_family == AF_INET6 && network == "tcp") setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        if (::bind(s, reinterpret_cast<sockaddr*>(&a.sa), a.len) < 0 || ::listen(s, listen_backlog()) < 0) {
            last = errno_error(errno);
            ::close(s);
            continue;
        }
        auto fd = make_ptr<netfd>(s, a.sa.ss_family, net);
        if (error err = fd->init()) return {nullptr, op_error("listen", net, {}, string(address), err)};
        sockaddr_storage la;
        socklen_t lalen = sizeof(la);
        getsockname(s, reinterpret_cast<sockaddr*>(&la), &lalen);
        fd->LocalAddr = sockaddr_string(&la);
        return {make_ptr<TCPListener>(fd), nullptr};
    }
    return {nullptr, op_error("listen", net, {}, string(address), last ? last : errors::New("no suitable address"))};
}

std::pair<ptr<Conn>, error> DialTimeout(std::string_view network, std::string_view address, time::Duration timeout) {
    string net(network);
    if (network != "tcp" && network != "tcp4" && network != "tcp6") {
        return {nullptr, op_error("dial", net, {}, string(address), errors::New("unknown network " + net))};
    }
    auto [addrs, rerr] = resolve(network, address, false);
    if (rerr) return {nullptr, op_error("dial", net, {}, string(address), rerr)};
    int64 deadline = timeout.ns > 0 ? runtime::nanotime() + timeout.ns : 0;

    error last;
    for (auto& a : addrs) {
        int s = ::socket(a.sa.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (s < 0) {
            last = errno_error(errno);
            continue;
        }
        auto fd = make_ptr<netfd>(s, a.sa.ss_family, net);
        if (error err = fd->init()) {
            last = err;
            continue;
        }
        if (error err = fd->connect(&a.sa, a.len, deadline)) {
            last = op_error("dial", net, {}, sockaddr_string(&a.sa), err);
            fd->Close();
            continue;
        }
        sockaddr_storage la;
        socklen_t lalen = sizeof(la);
        getsockname(s, reinterpret_cast<sockaddr*>(&la), &lalen);
        fd->LocalAddr = sockaddr_string(&la);
        fd->RemoteAddr = sockaddr_string(&a.sa);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return {make_ptr<TCPConn>(fd), nullptr};
    }
    return {nullptr, last ? last : op_error("dial", net, {}, string(address), errors::New("no suitable address"))};
}

std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address) {
    return DialTimeout(network, address, time::Duration(0));
}

// ---------------------------------------------------------------------------
// TCPConn

TCPConn::TCPConn(ptr<netfd> fd) : fd_(std::move(fd)) {}

TCPConn::~TCPConn() {
    // A dropped connection is closed, as a garbage-collected one would be
    // in Go eventually.
    fd_->Close();
}

std::pair<isize, error> TCPConn::Read(std::span<byte> p) {
    auto [n, err] = fd_->Read(p);
    return {n, op_error("read", fd_->network(), fd_->LocalAddr, fd_->RemoteAddr, err)};
}

std::pair<isize, error> TCPConn::Write(std::span<const byte> p) {
    auto [n, err] = fd_->Write(p);
    return {n, op_error("write", fd_->network(), fd_->LocalAddr, fd_->RemoteAddr, err)};
}

error TCPConn::Close() {
    return op_error("close", fd_->network(), fd_->LocalAddr, fd_->RemoteAddr, fd_->Close());
}

string TCPConn::LocalAddr() const { return fd_->LocalAddr; }
string TCPConn::RemoteAddr() const { return fd_->RemoteAddr; }

error TCPConn::SetDeadline(time::Time t) { return fd_->SetDeadline(t, 'r' + 'w'); }
error TCPConn::SetReadDeadline(time::Time t) { return fd_->SetDeadline(t, 'r'); }
error TCPConn::SetWriteDeadline(time::Time t) { return fd_->SetDeadline(t, 'w'); }

error TCPConn::CloseWrite() { return fd_->shutdown(SHUT_WR); }

error TCPConn::SetNoDelay(bool on) {
    int v = on ? 1 : 0;
    if (setsockopt(fd_->sysfd(), IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) < 0) return errno_error(errno);
    return nullptr;
}

error TCPConn::SetKeepAlive(bool on) {
    int v = on ? 1 : 0;
    if (setsockopt(fd_->sysfd(), SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v)) < 0) return errno_error(errno);
    return nullptr;
}

}  // namespace gopp::net
