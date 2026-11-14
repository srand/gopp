// Raw IP sockets, after Go's net/iprawsock.go and iprawsock_posix.go, and
// IP protocol numbers, after lookup.go (protocols, lookupProtocolMap) and
// lookup_unix.go (readProtocols, lookupProtocol).
#include "dial.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace gopp::net {

namespace {

constexpr std::size_t maxProtoLength = sizeof("RSVP-E2E-IGNORE") - 1 + 10;

// The minimal mappings, completed from /etc/protocols on first use.
using protocolMap = std::map<std::string, int, std::less<>>;

protocolMap loadProtocols() {
    protocolMap p = {
        {"icmp", 1}, {"igmp", 2}, {"tcp", 6}, {"udp", 17}, {"ipv6-icmp", 58},
    };
    runtime::blocking([&] {
        std::ifstream f("/etc/protocols");
        std::string line;
        while (std::getline(f, line)) {
            // tcp    6   TCP    # transmission control protocol
            if (auto h = line.find('#'); h != line.npos) line.resize(h);
            std::istringstream in(line);
            std::string name, num, alias;
            if (!(in >> name >> num)) continue;
            int proto = 0;
            std::size_t i = 0;
            for (; i < num.size() && std::isdigit(static_cast<unsigned char>(num[i])); i++) {
                proto = proto * 10 + (num[i] - '0');
            }
            if (i == 0) continue;
            p.emplace(name, proto);
            while (in >> alias) p.emplace(alias, proto);
        }
        return 0;
    });
    return p;
}

const protocolMap& protocols() {
    // Loaded under a sync::Once, not by the static's initializer: loading
    // parks the fiber, and fibers waiting on a static's guard block their
    // threads, and with them the Ps the loading fiber needs to finish.
    struct table {
        sync::Once once;
        protocolMap m;
    };
    static auto* t = new table;
    t->once.Do([] { t->m = loadProtocols(); });
    return t->m;
}

}  // namespace

std::pair<int, error> lookupProtocol(const ptr<context::Context>&, std::string_view name) {
    std::string lower(name.substr(0, std::min(name.size(), maxProtoLength)));
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto& m = protocols();
    auto it = m.find(lower);
    if (it == m.end() || lower.size() != name.size()) {
        return {0, make_ptr<AddrError>(string::fromStatic("unknown IP protocol specified"), string(name))};
    }
    return {it->second, nullptr};
}

// ---------------------------------------------------------------------------
// IPConn

namespace {

ptr<IPAddr> asIp(const ptr<Addr>& a) { return ptr<IPAddr>(dynamic_cast<IPAddr*>(a.get())); }

// Go's stripIPv4Header: an IPv4 raw socket's reads start with the IP
// header.
isize stripIPv4Header(isize n, std::span<byte> b) {
    if (b.size() < 20) return n;
    std::size_t l = std::size_t(b[0] & 0x0f) << 2;
    if (20 > l || l > b.size()) return n;
    if (b[0] >> 4 != 4) return n;
    std::memmove(b.data(), b.data() + l, b.size() - l);
    return n - isize(l);
}

}  // namespace

std::tuple<isize, ptr<IPAddr>, error> IPConn::ReadFromIP(std::span<byte> b) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, {}, 0, &from);
    ptr<IPAddr> addr = asIp(sockaddrToAddr(from, SOCK_RAW));
    if (from.ss_family == AF_INET) n = stripIPv4Header(n, b);
    if (err) return {n, addr, opError("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    return {n, addr, nullptr};
}

std::tuple<isize, ptr<net::Addr>, error> IPConn::ReadFrom(std::span<byte> b) {
    auto [n, addr, err] = ReadFromIP(b);
    return {n, addr, err};
}

std::tuple<isize, isize, int, ptr<IPAddr>, error> IPConn::ReadMsgIP(std::span<byte> b, std::span<byte> oob) {
    sockaddr_storage from;
    auto [n, oobn, flags, err] = fd_->readMsg(b, oob, 0, &from);
    ptr<IPAddr> addr = asIp(sockaddrToAddr(from, SOCK_RAW));
    if (err) return {n, oobn, flags, addr, opError("read", fd_->network(), fd_->laddr, fd_->raddr, err)};
    return {n, oobn, flags, addr, nullptr};
}

std::pair<isize, error> IPConn::WriteToIP(std::span<const byte> b, const ptr<IPAddr>& addr) {
    auto fail = [&](error err) {
        return std::pair<isize, error>{0, opError("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->raddr) return fail(ErrWriteToConnected);
    if (!addr) return fail(errMissingAddress);
    sockaddr_storage sa;
    socklen_t len;
    if (error err = addrSockaddr(addr.get(), fd_->family(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, {}, &sa, len);
    if (err) return {n, opError("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, nullptr};
}

std::pair<isize, error> IPConn::WriteTo(std::span<const byte> b, const ptr<net::Addr>& addr) {
    auto a = asIp(addr);
    if (!a) return {0, opError("write", fd_->network(), fd_->laddr, addr, errnoError(EINVAL))};
    return WriteToIP(b, a);
}

std::tuple<isize, isize, error> IPConn::WriteMsgIP(std::span<const byte> b, std::span<const byte> oob,
                                                   const ptr<IPAddr>& addr) {
    auto fail = [&](error err) {
        return std::tuple<isize, isize, error>{0, 0, opError("write", fd_->network(), fd_->laddr, addr, err)};
    };
    if (fd_->raddr) return fail(ErrWriteToConnected);
    if (!addr) return fail(errMissingAddress);
    sockaddr_storage sa;
    socklen_t len;
    if (error err = addrSockaddr(addr.get(), fd_->family(), &sa, &len)) return fail(err);
    auto [n, oobn, err] = fd_->writeMsg(b, oob, &sa, len);
    if (err) return {n, oobn, opError("write", fd_->network(), fd_->laddr, addr, err)};
    return {n, oobn, nullptr};
}

std::pair<ptr<IPConn>, error> sysDialer::dialIP(const ptr<context::Context>& ctx, const IPAddr* laddr,
                                                const IPAddr* raddr) {
    auto [afnet, proto, err] = parseNetwork(ctx, network.view(), true);
    if (err) return {nullptr, err};
    if (afnet != "ip" && afnet != "ip4" && afnet != "ip6") return {nullptr, make_ptr<UnknownNetworkError>(network)};
    auto [fd, serr] = internetSocket(ctx, afnet.view(), laddr, raddr, SOCK_RAW, proto, "dial", ctrlCtxFn());
    if (serr) return {nullptr, serr};
    return {make_ptr<IPConn>(fd), nullptr};
}

std::pair<ptr<IPConn>, error> sysListener::listenIP(const ptr<context::Context>& ctx, const IPAddr* laddr) {
    auto [afnet, proto, err] = parseNetwork(ctx, network.view(), true);
    if (err) return {nullptr, err};
    if (afnet != "ip" && afnet != "ip4" && afnet != "ip6") return {nullptr, make_ptr<UnknownNetworkError>(network)};
    auto [fd, serr] = internetSocket(ctx, afnet.view(), laddr, nullptr, SOCK_RAW, proto, "listen", ctrlCtxFn());
    if (serr) return {nullptr, serr};
    return {make_ptr<IPConn>(fd), nullptr};
}

std::pair<ptr<IPConn>, error> DialIP(std::string_view network, const ptr<IPAddr>& laddr, const ptr<IPAddr>& raddr) {
    string net(network);
    if (!raddr) return {nullptr, opError("dial", net, laddr, nullptr, errMissingAddress)};
    sysDialer sd{.d = {}, .network = net, .address = raddr->String()};
    auto [c, err] = sd.dialIP(context::Background(), laddr.get(), raddr.get());
    if (err) return {nullptr, opError("dial", net, laddr, raddr, err)};
    return {c, nullptr};
}

std::pair<ptr<IPConn>, error> ListenIP(std::string_view network, const ptr<IPAddr>& laddr0) {
    string net(network);
    ptr<IPAddr> laddr = laddr0 ? laddr0 : make_ptr<IPAddr>();
    sysListener sl{.lc = {}, .network = net, .address = laddr->String()};
    auto [c, err] = sl.listenIP(context::Background(), laddr.get());
    if (err) return {nullptr, opError("listen", net, nullptr, laddr0, err)};
    return {c, nullptr};
}

}  // namespace gopp::net
