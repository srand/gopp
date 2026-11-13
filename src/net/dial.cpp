// Dialer and ListenConfig, after Go's net/dial.go and the address list
// helpers of ipsock.go.
#include "dial.hpp"
#include "dnsclient.hpp"
#include "ipsock.hpp"

#include <gopp/runtime.hpp>

#include <cstdlib>
#include <utility>

namespace gopp::net {

namespace {

// Runs f when it goes out of scope (Go's defer).
template <typename F>
struct defer_t {
    F f;
    ~defer_t() { f(); }
};
template <typename F>
defer_t(F) -> defer_t<F>;

// GODEBUG=multipathtcp: "0" turns listening off, "1" turns dialing on,
// "3" both (Go's multipathtcp setting).
std::string_view multipathtcp() {
    static std::string v = [] {
        const char* g = std::getenv("GODEBUG");
        if (!g) return std::string();
        std::string_view s(g);
        while (!s.empty()) {
            auto comma = s.find(',');
            std::string_view kv = s.substr(0, comma);
            if (kv.starts_with("multipathtcp=")) return std::string(kv.substr(13));
            if (comma == s.npos) break;
            s.remove_prefix(comma + 1);
        }
        return std::string();
    }();
    return v;
}

time::Time minNonzeroTime(time::Time a, time::Time b) {
    if (a.IsZero()) return b;
    if (b.IsZero() || a.Before(b)) return a;
    return b;
}

// Go's Dialer.deadline: the earliest of now+Timeout, ctx's deadline and
// Deadline.
time::Time dialerDeadline(const Dialer& d, const ptr<context::Context>& ctx, time::Time now) {
    time::Time earliest;
    if (d.Timeout.ns != 0) earliest = now.Add(d.Timeout);  // including negative, for historical reasons
    if (auto cd = ctx->Deadline()) earliest = minNonzeroTime(earliest, *cd);
    return minNonzeroTime(earliest, d.Deadline);
}

time::Duration fallbackDelay(const Dialer& d) {
    if (d.FallbackDelay.ns > 0) return d.FallbackDelay;
    return 300 * time::Millisecond;
}

bool dualStack(const Dialer& d) { return d.FallbackDelay.ns >= 0; }

// Go's Dialer.dialCtx: ctx with the dialer's deadline and Cancel.
std::pair<ptr<context::Context>, context::CancelFunc> dialCtx(const Dialer& d, ptr<context::Context> ctx) {
    if (!ctx) panic("nil context");
    time::Time deadline = dialerDeadline(d, ctx, time::Now());
    context::CancelFunc cancel1, cancel2;
    if (!deadline.IsZero()) {
        auto cd = ctx->Deadline();
        if (!cd || deadline.Before(*cd)) std::tie(ctx, cancel1) = context::WithDeadline(ctx, deadline);
    }
    if (auto oldCancel = d.Cancel) {
        ptr<context::Context> subCtx;
        std::tie(subCtx, cancel2) = context::WithCancel(ctx);
        auto c2 = cancel2;
        go([oldCancel, subCtx, c2] {
            select(recv(oldCancel, [&](empty) { c2(); }), recv(subCtx->Done(), [](empty) {}));
        });
        ctx = subCtx;
    }
    return {ctx, [cancel1, cancel2] {
                if (cancel1) cancel1();
                if (cancel2) cancel2();
            }};
}

bool isNotIPv4(const Addr* a) { return !isIPv4(a); }

bool matchAddrFamily(const IP& ip, const IP& x) {
    return (!ip.To4().empty() && !x.To4().empty()) ||
           (!ip.To16().empty() && ip.To4().empty() && !x.To16().empty() && x.To4().empty());
}

bool isWildcardIP(const Addr* a) {
    ip_port ip = ip_port_of(a);
    return ip.ip.empty() || ip.ip.IsUnspecified();
}

}  // namespace

// Go's lookupProtocol, in iprawsock's file once raw IP sockets are here.
std::pair<int, error> lookupProtocol(const ptr<context::Context>& ctx, std::string_view name);

// Go's partialDeadline: a share of the time left for one of
// addrsRemaining addresses, at least 2s if there's that much.
std::pair<time::Time, error> partialDeadline(time::Time now, time::Time deadline, int addrsRemaining) {
    if (deadline.IsZero()) return {deadline, nullptr};
    time::Duration timeRemaining = deadline.Sub(now);
    if (timeRemaining.ns <= 0) return {time::Time{}, errTimeout};
    time::Duration timeout = timeRemaining / addrsRemaining;
    constexpr time::Duration saneMinimum = 2 * time::Second;
    if (timeout < saneMinimum) timeout = timeRemaining < saneMinimum ? timeRemaining : saneMinimum;
    return {now.Add(timeout), nullptr};
}

bool isIPv4(const Addr* a) {
    ip_port ip = ip_port_of(a);
    return ip.ok && !ip.ip.To4().empty();
}

std::pair<addrList, addrList> partition(const addrList& addrs, bool (*strategy)(const Addr*)) {
    addrList primaries, fallbacks;
    bool primaryLabel = false;
    for (std::size_t i = 0; i < addrs.size(); i++) {
        bool label = strategy(addrs[i].get());
        if (i == 0 || label == primaryLabel) {
            primaryLabel = label;
            primaries.push_back(addrs[i]);
        } else {
            fallbacks.push_back(addrs[i]);
        }
    }
    return {primaries, fallbacks};
}

ptr<Addr> first(const addrList& addrs, bool (*strategy)(const Addr*)) {
    for (auto& a : addrs) {
        if (strategy(a.get())) return a;
    }
    return addrs.front();
}

ptr<Addr> forResolve(const addrList& addrs, std::string_view network, std::string_view addr) {
    bool want6 = false;
    if (network == "ip") want6 = addr.find(':') != addr.npos;
    else if (network == "tcp" || network == "udp") want6 = addr.find('[') != addr.npos;
    return want6 ? first(addrs, isNotIPv4) : first(addrs, isIPv4);
}

std::tuple<string, int, error> parseNetwork(const ptr<context::Context>& ctx, std::string_view network,
                                            bool needsProto) {
    auto i = network.rfind(':');
    if (i == network.npos) {
        if (network == "tcp" || network == "tcp4" || network == "tcp6" || network == "udp" || network == "udp4" ||
            network == "udp6" || network == "unix" || network == "unixgram" || network == "unixpacket") {
            return {string(network), 0, nullptr};
        }
        if (network == "ip" || network == "ip4" || network == "ip6") {
            if (needsProto) return {{}, 0, make_ptr<UnknownNetworkError>(string(network))};
            return {string(network), 0, nullptr};
        }
        return {{}, 0, make_ptr<UnknownNetworkError>(string(network))};
    }
    std::string_view afnet = network.substr(0, i);
    if (afnet == "ip" || afnet == "ip4" || afnet == "ip6") {
        std::string_view protostr = network.substr(i + 1);
        auto [proto, j, ok] = dtoi(protostr);
        if (!ok || j != protostr.size()) {
            error err;
            std::tie(proto, err) = lookupProtocol(ctx, protostr);
            if (err) return {{}, 0, err};
        }
        return {string(afnet), proto, nullptr};
    }
    return {{}, 0, make_ptr<UnknownNetworkError>(string(network))};
}

std::pair<addrList, error> resolveAddrList(Resolver* r, const ptr<context::Context>& ctx, std::string_view op,
                                           std::string_view network, std::string_view addr, const Addr* hint) {
    auto [afnet, proto, err] = parseNetwork(ctx, network, true);
    if (err) return {{}, err};
    if (op == "dial" && addr.empty()) return {{}, errMissingAddress};
    std::string_view af = afnet.view();
    if (af == "unix" || af == "unixgram" || af == "unixpacket") {
        auto [ua, err] = ResolveUnixAddr(af, addr);
        if (err) return {{}, err};
        if (op == "dial" && hint && ua->Network() != hint->Network()) {
            return {{}, make_ptr<AddrError>(string::from_static("mismatched local address type"), hint->String())};
        }
        return {{ua}, nullptr};
    }
    auto [addrs, aerr] = internetAddrList(r, ctx, af, addr);
    if (aerr || op != "dial" || !hint) return {addrs, aerr};

    // Only the addresses of the local address's family, unless one of
    // them is a wildcard.
    bool wildcard = isWildcardIP(hint);
    IP hintIP = ip_port_of(hint).ip;
    addrList naddrs;
    for (auto& a : addrs) {
        if (a->Network() != hint->Network()) {
            return {{}, make_ptr<AddrError>(string::from_static("mismatched local address type"), hint->String())};
        }
        if (!wildcard && !isWildcardIP(a.get()) && !matchAddrFamily(ip_port_of(a.get()).ip, hintIP)) continue;
        naddrs.push_back(a);
    }
    if (naddrs.empty()) {
        return {{}, make_ptr<AddrError>(string::from_static("no suitable address found"), hint->String())};
    }
    return {naddrs, nullptr};
}

// ---------------------------------------------------------------------------
// sysDialer

namespace {

ControlContextFunc dialerCtrl(const Dialer& d) {
    if (d.ControlContext) return d.ControlContext;
    if (d.Control) {
        auto f = d.Control;
        return [f](const ptr<context::Context>&, std::string_view network, std::string_view address,
                   syscall::RawConn& c) { return f(network, address, c); };
    }
    return {};
}

}  // namespace

ControlContextFunc sysDialer::ctrlCtxFn() const { return dialerCtrl(d); }

ControlContextFunc sysListener::ctrlCtxFn() const {
    if (!lc.Control) return {};
    auto f = lc.Control;
    return [f](const ptr<context::Context>&, std::string_view network, std::string_view address, syscall::RawConn& c) {
        return f(network, address, c);
    };
}

std::pair<ptr<Conn>, error> sysDialer::dialParallel(const ptr<context::Context>& ctx, const addrList& primaries,
                                                    const addrList& fallbacks) {
    if (fallbacks.empty()) return dialSerial(ctx, primaries);

    auto returned = make_chan<empty>();
    defer_t closeReturned{[&] { returned.close(); }};

    struct dialResult {
        ptr<Conn> conn;
        error err;
        bool primary = false;
        bool done = false;
    };
    auto results = make_chan<dialResult>();  // unbuffered

    // The racers share the dialer through a copy.
    auto self = std::make_shared<sysDialer>(*this);
    auto startRacer = [self, results, returned, primaries, fallbacks](ptr<context::Context> rctx, bool primary) {
        go([self, results, returned, primaries, fallbacks, rctx, primary] {
            auto [c, err] = self->dialSerial(rctx, primary ? primaries : fallbacks);
            select(send(results, dialResult{c, err, primary, true}, [] {}), recv(returned, [&](empty) {
                       if (c) c->Close();
                   }));
        });
    };

    dialResult primary, fallback;

    // Start the main racer.
    auto [primaryCtx, primaryCancel] = context::WithCancel(ctx);
    defer_t cancelPrimary{[&] { primaryCancel(); }};
    startRacer(primaryCtx, true);

    // Start the timer for the fallback racer.
    auto fallbackTimer = time::NewTimer(fallbackDelay(d));
    defer_t stopTimer{[&] { fallbackTimer->Stop(); }};
    std::vector<context::CancelFunc> fallbackCancels;
    defer_t cancelFallbacks{[&] {
        for (auto& c : fallbackCancels) c();
    }};

    for (;;) {
        std::optional<std::pair<ptr<Conn>, error>> ret;
        select(recv(fallbackTimer->C,
                    [&](time::Time) {
                        auto [fallbackCtx, fallbackCancel] = context::WithCancel(ctx);
                        fallbackCancels.push_back(fallbackCancel);
                        startRacer(fallbackCtx, false);
                    }),
               recv(results, [&](dialResult res) {
                   if (!res.err) {
                       ret.emplace(res.conn, nullptr);
                       return;
                   }
                   if (res.primary) primary = res;
                   else fallback = res;
                   if (primary.done && fallback.done) {
                       ret.emplace(nullptr, primary.err);
                       return;
                   }
                   // The primary failed: start the fallback now.
                   if (res.primary && fallbackTimer->Stop()) fallbackTimer->Reset(time::Duration(0));
               }));
        if (ret) return *ret;
    }
}

std::pair<ptr<Conn>, error> sysDialer::dialSerial(const ptr<context::Context>& ctx, const addrList& ras) {
    error firstErr;  // the error from the first address is most relevant
    std::vector<context::CancelFunc> cancels;
    defer_t cancelAll{[&] {
        for (auto& c : cancels) c();
    }};
    for (std::size_t i = 0; i < ras.size(); i++) {
        const auto& ra = ras[i];
        if (error cerr = ctx->Err()) return {nullptr, op_error("dial", network, d.LocalAddr, ra, mapErr(cerr))};
        ptr<context::Context> dctx = ctx;
        if (auto deadline = ctx->Deadline()) {
            auto [pd, err] = partialDeadline(time::Now(), *deadline, int(ras.size() - i));
            if (err) {
                // Ran out of time.
                if (!firstErr) firstErr = op_error("dial", network, d.LocalAddr, ra, err);
                break;
            }
            if (pd.Before(*deadline)) {
                context::CancelFunc cancel;
                std::tie(dctx, cancel) = context::WithDeadline(ctx, pd);
                cancels.push_back(cancel);
            }
        }
        auto [c, err] = dialSingle(dctx, ra);
        if (!err) return {c, nullptr};
        if (!firstErr) firstErr = err;
    }
    if (!firstErr) firstErr = op_error("dial", network, nullptr, nullptr, errMissingAddress);
    return {nullptr, firstErr};
}

std::pair<ptr<Conn>, error> sysDialer::dialSingle(const ptr<context::Context>& ctx, const ptr<Addr>& ra) {
    const Addr* la = d.LocalAddr.get();
    ptr<Conn> c;
    error err;
    if (auto* t = dynamic_cast<const TCPAddr*>(ra.get())) {
        auto* tla = dynamic_cast<const TCPAddr*>(la);
        std::tie(c, err) = d.MultipathTCP() ? dialMPTCP(ctx, tla, t) : dialTCP(ctx, tla, t);
    } else if (auto* u = dynamic_cast<const UDPAddr*>(ra.get())) {
        std::tie(c, err) = dialUDP(ctx, dynamic_cast<const UDPAddr*>(la), u);
    } else if (auto* ip = dynamic_cast<const IPAddr*>(ra.get())) {
        std::tie(c, err) = dialIP(ctx, dynamic_cast<const IPAddr*>(la), ip);
    } else if (auto* x = dynamic_cast<const UnixAddr*>(ra.get())) {
        std::tie(c, err) = dialUnix(ctx, dynamic_cast<const UnixAddr*>(la), x);
    } else {
        return {nullptr, op_error("dial", network, d.LocalAddr, ra,
                                  make_ptr<AddrError>(string::from_static("unexpected address type"), address))};
    }
    if (err) return {nullptr, op_error("dial", network, d.LocalAddr, ra, err)};
    return {c, nullptr};
}

// ---------------------------------------------------------------------------
// Dialer

bool Dialer::MultipathTCP() const {
    if (mptcpStatus == 1) return true;
    if (mptcpStatus == 2) return false;
    return multipathtcp() == "1" || multipathtcp() == "3";
}

void Dialer::SetMultipathTCP(bool use) { mptcpStatus = use ? 1 : 2; }

std::pair<ptr<Conn>, error> Dialer::Dial(std::string_view network, std::string_view address) {
    return DialContext(context::Background(), network, address);
}

std::pair<ptr<Conn>, error> Dialer::DialContext(const ptr<context::Context>& ctx0, std::string_view network,
                                                std::string_view address) {
    auto [ctx, cancel] = dialCtx(*this, ctx0);
    defer_t cancelCtx{[&] { cancel(); }};
    string net(network);
    auto [addrs, err] = resolveAddrList(Resolver.get(), ctx, "dial", network, address, LocalAddr.get());
    if (err) return {nullptr, op_error("dial", net, nullptr, nullptr, err)};
    sysDialer sd{.d = *this, .network = net, .address = string(address)};
    addrList primaries, fallbacks;
    if (dualStack(*this) && network == "tcp") std::tie(primaries, fallbacks) = partition(addrs, isIPv4);
    else primaries = addrs;
    return sd.dialParallel(ctx, primaries, fallbacks);
}

std::pair<ptr<TCPConn>, error> Dialer::DialTCP(const ptr<context::Context>& ctx0, std::string_view network,
                                               netip::AddrPort laddr, netip::AddrPort raddr) {
    auto [ctx, cancel] = dialCtx(*this, ctx0);
    defer_t cancelCtx{[&] { cancel(); }};
    string net(network);
    ptr<TCPAddr> la = laddr.IsValid() ? TCPAddrFromAddrPort(laddr) : nullptr;
    ptr<TCPAddr> ra = raddr.IsValid() ? TCPAddrFromAddrPort(raddr) : nullptr;
    if (network != "tcp" && network != "tcp4" && network != "tcp6") {
        return {nullptr, op_error("dial", net, la, ra, make_ptr<UnknownNetworkError>(net))};
    }
    if (!ra) return {nullptr, op_error("dial", net, la, nullptr, errMissingAddress)};
    sysDialer sd{.d = *this, .network = net, .address = ra->String()};
    auto [c, err] = MultipathTCP() ? sd.dialMPTCP(ctx, la.get(), ra.get()) : sd.dialTCP(ctx, la.get(), ra.get());
    if (err) return {nullptr, op_error("dial", net, la, ra, err)};
    return {c, nullptr};
}

std::pair<ptr<UDPConn>, error> Dialer::DialUDP(const ptr<context::Context>& ctx0, std::string_view network,
                                               netip::AddrPort laddr, netip::AddrPort raddr) {
    auto [ctx, cancel] = dialCtx(*this, ctx0);
    defer_t cancelCtx{[&] { cancel(); }};
    string net(network);
    ptr<UDPAddr> la = laddr.IsValid() ? UDPAddrFromAddrPort(laddr) : nullptr;
    ptr<UDPAddr> ra = raddr.IsValid() ? UDPAddrFromAddrPort(raddr) : nullptr;
    if (network != "udp" && network != "udp4" && network != "udp6") {
        return {nullptr, op_error("dial", net, la, ra, make_ptr<UnknownNetworkError>(net))};
    }
    if (!ra) return {nullptr, op_error("dial", net, la, nullptr, errMissingAddress)};
    sysDialer sd{.d = *this, .network = net, .address = ra->String()};
    auto [c, err] = sd.dialUDP(ctx, la.get(), ra.get());
    if (err) return {nullptr, op_error("dial", net, la, ra, err)};
    return {c, nullptr};
}

std::pair<ptr<UnixConn>, error> Dialer::DialUnix(const ptr<context::Context>& ctx0, std::string_view network,
                                                 const ptr<UnixAddr>& laddr, const ptr<UnixAddr>& raddr) {
    auto [ctx, cancel] = dialCtx(*this, ctx0);
    defer_t cancelCtx{[&] { cancel(); }};
    string net(network);
    if (network != "unix" && network != "unixgram" && network != "unixpacket") {
        return {nullptr, op_error("dial", net, laddr, raddr, make_ptr<UnknownNetworkError>(net))};
    }
    sysDialer sd{.d = *this, .network = net, .address = raddr ? raddr->String() : string()};
    auto [c, err] = sd.dialUnix(ctx, laddr.get(), raddr.get());
    if (err) return {nullptr, op_error("dial", net, laddr, raddr, err)};
    return {c, nullptr};
}

std::pair<ptr<IPConn>, error> Dialer::DialIP(const ptr<context::Context>& ctx0, std::string_view network,
                                             netip::Addr laddr, netip::Addr raddr) {
    auto [ctx, cancel] = dialCtx(*this, ctx0);
    defer_t cancelCtx{[&] { cancel(); }};
    string net(network);
    // Go's ipAddrFromAddr: an invalid Addr is a nil IP.
    auto la = make_ptr<IPAddr>(IP(laddr.AsSlice()), laddr.Zone());
    auto ra = make_ptr<IPAddr>(IP(raddr.AsSlice()), raddr.Zone());
    sysDialer sd{.d = *this, .network = net, .address = ra->String()};
    auto [c, err] = sd.dialIP(ctx, la.get(), ra.get());
    if (err) return {nullptr, op_error("dial", net, la, ra, err)};
    return {c, nullptr};
}

std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view address) {
    Dialer d;
    return d.Dial(network, address);
}

std::pair<ptr<Conn>, error> DialTimeout(std::string_view network, std::string_view address, time::Duration timeout) {
    Dialer d{.Timeout = timeout};
    return d.Dial(network, address);
}

// ---------------------------------------------------------------------------
// ListenConfig

bool ListenConfig::MultipathTCP() const {
    if (mptcpStatus == 1) return true;
    if (mptcpStatus == 2) return false;
    return !(multipathtcp() == "0" || multipathtcp() == "3");
}

void ListenConfig::SetMultipathTCP(bool use) { mptcpStatus = use ? 1 : 2; }

std::pair<ptr<net::Listener>, error> ListenConfig::Listen(const ptr<context::Context>& ctx, std::string_view network,
                                                          std::string_view address) {
    string net(network);
    auto [addrs, err] = resolveAddrList(nullptr, ctx, "listen", network, address, nullptr);
    if (err) return {nullptr, op_error("listen", net, nullptr, nullptr, err)};
    sysListener sl{.lc = *this, .network = net, .address = string(address)};
    ptr<Addr> la = first(addrs, isIPv4);
    ptr<net::Listener> l;
    if (auto* t = dynamic_cast<const TCPAddr*>(la.get())) {
        std::tie(l, err) = MultipathTCP() ? sl.listenMPTCP(ctx, t) : sl.listenTCP(ctx, t);
    } else if (auto* u = dynamic_cast<const UnixAddr*>(la.get())) {
        std::tie(l, err) = sl.listenUnix(ctx, u);
    } else {
        return {nullptr, op_error("listen", net, nullptr, la,
                                  make_ptr<AddrError>(string::from_static("unexpected address type"), string(address)))};
    }
    if (err) return {nullptr, op_error("listen", net, nullptr, la, err)};
    return {l, nullptr};
}

std::pair<ptr<net::PacketConn>, error> ListenConfig::ListenPacket(const ptr<context::Context>& ctx,
                                                                 std::string_view network, std::string_view address) {
    string net(network);
    auto [addrs, err] = resolveAddrList(nullptr, ctx, "listen", network, address, nullptr);
    if (err) return {nullptr, op_error("listen", net, nullptr, nullptr, err)};
    sysListener sl{.lc = *this, .network = net, .address = string(address)};
    ptr<Addr> la = first(addrs, isIPv4);
    ptr<net::PacketConn> c;
    if (auto* u = dynamic_cast<const UDPAddr*>(la.get())) {
        std::tie(c, err) = sl.listenUDP(ctx, u);
    } else if (auto* ip = dynamic_cast<const IPAddr*>(la.get())) {
        std::tie(c, err) = sl.listenIP(ctx, ip);
    } else if (auto* x = dynamic_cast<const UnixAddr*>(la.get())) {
        std::tie(c, err) = sl.listenUnixgram(ctx, x);
    } else {
        return {nullptr, op_error("listen", net, nullptr, la,
                                  make_ptr<AddrError>(string::from_static("unexpected address type"), string(address)))};
    }
    if (err) return {nullptr, op_error("listen", net, nullptr, la, err)};
    return {c, nullptr};
}

std::pair<ptr<Listener>, error> Listen(std::string_view network, std::string_view address) {
    ListenConfig lc;
    return lc.Listen(context::Background(), network, address);
}

std::pair<ptr<PacketConn>, error> ListenPacket(std::string_view network, std::string_view address) {
    ListenConfig lc;
    return lc.ListenPacket(context::Background(), network, address);
}

}  // namespace gopp::net
