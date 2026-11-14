// The DNS client and Resolver, ported from Go 1.27.3's net/dnsclient.go,
// dnsclient_unix.go, lookup.go, lookup_unix.go, the lookups of
// ipsock.go and internal/singleflight.
#include "dnsclient.hpp"
#include "dial.hpp"
#include "fd.hpp"
#include "ipsock.hpp"

#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <cstdlib>
#include <random>

namespace gopp::net {

// ---------------------------------------------------------------------------
// Errors

namespace {

// Go's notFoundError: newDNSError sets IsNotFound for it.
struct notFoundError final : errorInterface {
    string s;
    explicit notFoundError(std::string_view s_) : s(s_) {}
    string Error() const override { return s; }
};

// Go's temporaryError.
struct temporaryError final : errorInterface {
    string s;
    explicit temporaryError(std::string_view s_) : s(s_) {}
    string Error() const override { return s; }
};

}  // namespace

const error errNoSuchHost = make_ptr<notFoundError>("no such host");
const error errUnknownPort = make_ptr<notFoundError>("unknown port");
const error errLameReferral = errors::New("lame referral");
const error errCannotUnmarshalDNSMessage = errors::New("cannot unmarshal DNS message");
const error errCannotMarshalDNSMessage = errors::New("cannot marshal DNS message");
const error errServerMisbehaving = errors::New("server misbehaving");
const error errInvalidDNSResponse = errors::New("invalid DNS response");
const error errNoAnswerFromDNSServer = errors::New("no answer from DNS server");
const error errServerTemporarilyMisbehaving = make_ptr<temporaryError>("server misbehaving");
const char* const errMalformedDNSRecordsDetail = "DNS response contained records which contain invalid names";

bool isTemporaryError(const error& err) {
    const errorInterface* e = err.get();
    if (!e) return false;
    if (dynamic_cast<const temporaryError*>(e)) return true;
    if (auto* d = dynamic_cast<const DNSError*>(e)) return d->Temporary();
    if (e == errTimeout.get() || errors::Is(err, os::ErrDeadlineExceeded)) return true;
    if (auto* o = dynamic_cast<const OpError*>(e)) {
        // Go's OpError.Temporary: through a SyscallError to its Errno.
        if (auto* s = dynamic_cast<const os::SyscallError*>(o->Err.get())) return isTemporaryError(s->Err);
        if (o->Op == "accept") {
            if (auto* n = dynamic_cast<const Errno*>(o->Err.get()); n && (n->Code == ECONNRESET || n->Code == ECONNABORTED)) {
                return true;
            }
        }
        return isTemporaryError(o->Err);
    }
    if (auto* n = dynamic_cast<const Errno*>(e)) {
        switch (n->Code) {
        case EINTR:
        case EMFILE:
        case ENFILE:
        case ECONNRESET:
        case ECONNABORTED:
        case EAGAIN:
        case ETIMEDOUT:
            return true;
        }
        return n->Timeout();
    }
    return false;
}

namespace {

// Whether err is a net.Error, whose Timeout and Temporary newDNSError
// copies.
bool isNetError(const error& err) {
    const errorInterface* e = err.get();
    return dynamic_cast<const temporaryError*>(e) || dynamic_cast<const DNSError*>(e) ||
           dynamic_cast<const OpError*>(e) || dynamic_cast<const Errno*>(e) || e == errTimeout.get() ||
           errors::Is(err, os::ErrDeadlineExceeded) || dynamic_cast<const AddrError*>(e) ||
           dynamic_cast<const UnknownNetworkError*>(e) || dynamic_cast<const InvalidAddrError*>(e) ||
           dynamic_cast<const ParseError*>(e);
}

}  // namespace

ptr<DNSError> newDNSError(const error& err, std::string_view name, std::string_view server) {
    auto e = make_ptr<DNSError>();
    if (isNetError(err)) {
        e->IsTimeout = err->Timeout();
        e->IsTemporary = isTemporaryError(err);
    }
    // At this time, the only errors we wrap are context errors, to allow
    // users to check for canceled/timed out requests.
    if (errors::Is(err, context::DeadlineExceeded) || errors::Is(err, context::Canceled)) e->UnwrapErr = err;
    e->Err = err.Error();
    e->Name = string(name);
    e->Server = string(server);
    e->IsNotFound = dynamic_cast<const notFoundError*>(err.get()) != nullptr;
    return e;
}

namespace {

ptr<DNSError> dnsError(std::string_view err, std::string_view name, std::string_view server = {}) {
    auto e = make_ptr<DNSError>();
    e->Err = string(err);
    e->Name = string(name);
    e->Server = string(server);
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// dnsclient.go

int randInt() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    return int(uint64(rng()) >> 33);  // a non-negative int
}

int randIntn(int n) { return randInt() % n; }

std::pair<std::string, error> reverseaddr(std::string_view addr) {
    IP ip = ParseIP(addr);
    if (ip.empty()) return {{}, dnsError("unrecognized address", addr)};
    if (!ip.To4().empty()) {
        return {std::to_string(ip[15]) + "." + std::to_string(ip[14]) + "." + std::to_string(ip[13]) + "." +
                    std::to_string(ip[12]) + ".in-addr.arpa.",
                nullptr};
    }
    // Must be IPv6.
    constexpr std::string_view hexDigit = "0123456789abcdef";
    std::string buf;
    for (int i = int(ip.size()) - 1; i >= 0; i--) {
        byte v = ip[std::size_t(i)];
        buf += hexDigit[v & 0xF];
        buf += '.';
        buf += hexDigit[v >> 4];
        buf += '.';
    }
    // Append "ip6.arpa." and return (buf already has the final .).
    return {buf + "ip6.arpa.", nullptr};
}

bool equalASCIIName(const dnsmessage::Name& x, const dnsmessage::Name& y) {
    if (x.Length != y.Length) return false;
    for (int i = 0; i < int(x.Length); i++) {
        byte a = x.Data[std::size_t(i)], b = y.Data[std::size_t(i)];
        if ('A' <= a && a <= 'Z') a += 0x20;
        if ('A' <= b && b <= 'Z') b += 0x20;
        if (a != b) return false;
    }
    return true;
}

bool isDomainName(std::string_view s) {
    // The root domain name is valid. See golang.org/issue/45715.
    if (s == ".") return true;
    // See RFC 1035, RFC 3696. Presentation format has dots before every
    // label except the first, and the end of the name may have a trailing
    // dot; the wire format max length of 255 bytes is 254 here, 253 when
    // the name isn't rooted.
    std::size_t l = s.size();
    if (l == 0 || l > 254 || (l == 254 && s[l - 1] != '.')) return false;
    char last = '.';
    bool nonNumeric = false;  // true once we've seen a letter or hyphen
    int partlen = 0;
    for (char c : s) {
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_') {
            nonNumeric = true;
            partlen++;
        } else if ('0' <= c && c <= '9') {
            // fine
            partlen++;
        } else if (c == '-') {
            // Byte before dash cannot be dot.
            if (last == '.') return false;
            partlen++;
            nonNumeric = true;
        } else if (c == '.') {
            // Byte before dot cannot be dot, dash.
            if (last == '.' || last == '-') return false;
            if (partlen > 63 || partlen == 0) return false;
            partlen = 0;
        } else {
            return false;
        }
        last = c;
    }
    if (last == '-' || partlen > 63) return false;
    return nonNumeric;
}

std::string absDomainName(std::string_view s) {
    if (s.find('.') != s.npos && s.back() != '.') return std::string(s) + ".";
    return std::string(s);
}

namespace {

}  // namespace

// Go's byPriorityWeight.shuffleByWeight (RFC 2782).
void shuffleByWeight(ptr<SRV>* addrs, std::size_t n) {
    int sum = 0;
    for (std::size_t i = 0; i < n; i++) sum += addrs[i]->Weight;
    while (sum > 0 && n > 1) {
        int s = 0;
        int r = randIntn(sum);
        for (std::size_t i = 0; i < n; i++) {
            s += addrs[i]->Weight;
            if (s > r) {
                if (i > 0) std::swap(addrs[0], addrs[i]);
                break;
            }
        }
        sum -= addrs[0]->Weight;
        addrs++;
        n--;
    }
}

void sortSRV(std::vector<ptr<SRV>>& addrs) {
    std::sort(addrs.begin(), addrs.end(), [](const ptr<SRV>& a, const ptr<SRV>& b) {
        if (a->Priority != b->Priority) return a->Priority < b->Priority;
        return a->Weight < b->Weight;
    });
    std::size_t i = 0;
    for (std::size_t j = 1; j < addrs.size(); j++) {
        if (addrs[i]->Priority != addrs[j]->Priority) {
            shuffleByWeight(addrs.data() + i, j - i);
            i = j;
        }
    }
    if (i < addrs.size()) shuffleByWeight(addrs.data() + i, addrs.size() - i);
}

void sortMX(std::vector<ptr<MX>>& s) {
    // Shuffle, then stable-sort by preference (RFC 5321).
    for (std::size_t i = 0; i < s.size(); i++) std::swap(s[i], s[std::size_t(randIntn(int(i) + 1))]);
    std::stable_sort(s.begin(), s.end(), [](const ptr<MX>& a, const ptr<MX>& b) { return a->Pref < b->Pref; });
}

// ---------------------------------------------------------------------------
// dnsclient_unix.go

namespace {

// Go's maxDNSPacketSize: the EDNS(0) payload size that avoids
// fragmentation (DNS Flag Day 2020).
constexpr int maxDNSPacketSize = 1232;

// GODEBUG=netedns0=0 turns off EDNS(0).
bool netedns0Off() {
    const char* g = std::getenv("GODEBUG");
    return g && std::string_view(g).find("netedns0=0") != std::string_view::npos;
}

struct request {
    uint16 id = 0;
    slice<byte> udpReq, tcpReq;
};

std::pair<request, error> newRequest(const dnsmessage::Question& q, bool ad) {
    request r;
    r.id = uint16(randInt());
    dnsmessage::Header h;
    h.ID = r.id;
    h.RecursionDesired = true;
    h.AuthenticData = ad;
    auto b = dnsmessage::NewBuilder(slice<byte>::make(2, 514), h);
    if (error err = b.StartQuestions()) return {{}, err};
    if (error err = b.Question(q)) return {{}, err};
    if (!netedns0Off()) {
        // Accept packets up to maxDNSPacketSize (RFC 6891).
        if (error err = b.StartAdditionals()) return {{}, err};
        dnsmessage::ResourceHeader rh;
        if (error err = rh.SetEDNS0(maxDNSPacketSize, dnsmessage::RCodeSuccess, false)) return {{}, err};
        if (error err = b.OPTResource(rh, dnsmessage::OPTResource{})) return {{}, err};
    }
    auto [tcpReq, err] = b.Finish();
    if (err) return {{}, err};
    r.udpReq = tcpReq.Sub(2);
    std::size_t l = tcpReq.size() - 2;
    tcpReq[0] = byte(l >> 8);
    tcpReq[1] = byte(l);
    r.tcpReq = tcpReq;
    return {r, nullptr};
}

bool checkResponse(uint16 reqID, const dnsmessage::Question& reqQues, const dnsmessage::Header& respHdr,
                   const dnsmessage::Question& respQues) {
    if (!respHdr.Response) return false;
    if (reqID != respHdr.ID) return false;
    if (reqQues.Type != respQues.Type || reqQues.Class != respQues.Class ||
        !equalASCIIName(reqQues.Name, respQues.Name)) {
        return false;
    }
    return true;
}

using roundTrip = std::tuple<dnsmessage::Parser, dnsmessage::Header, error>;

roundTrip dnsPacketRoundTrip(Conn& c, uint16 id, const dnsmessage::Question& query, const slice<byte>& b) {
    if (auto [n, err] = c.Write(b); err) return {{}, {}, err};
    for (;;) {
        auto buf = slice<byte>::make(maxDNSPacketSize);
        auto [n, err] = c.Read(buf);
        if (err) return {{}, {}, err};
        dnsmessage::Parser p;
        // Ignore invalid responses as they may be malicious forgery
        // attempts; instead continue waiting until timeout (RFC 5452).
        auto [h, perr] = p.Start(buf.Sub(0, std::size_t(n)));
        if (perr) continue;
        auto [q, qerr] = p.Question();
        if (qerr || !checkResponse(id, query, h, q)) continue;
        return {p, h, nullptr};
    }
}

roundTrip dnsStreamRoundTrip(Conn& c, uint16 id, const dnsmessage::Question& query, const slice<byte>& b) {
    if (auto [n, err] = c.Write(b); err) return {{}, {}, err};
    auto buf = slice<byte>::make(1280);  // a reasonable initial size for IP over Ethernet (RFC 4035)
    if (auto [n, err] = io::ReadFull(c, buf.span().first(2)); err) return {{}, {}, err};
    std::size_t l = std::size_t(buf[0]) << 8 | buf[1];
    if (l > buf.size()) buf = slice<byte>::make(l);
    auto [n, err] = io::ReadFull(c, buf.span().first(l));
    if (err) return {{}, {}, err};
    dnsmessage::Parser p;
    auto [h, perr] = p.Start(buf.Sub(0, std::size_t(n)));
    if (perr) return {{}, {}, errCannotUnmarshalDNSMessage};
    auto [q, qerr] = p.Question();
    if (qerr) return {{}, {}, errCannotUnmarshalDNSMessage};
    if (!checkResponse(id, query, h, q)) return {{}, {}, errInvalidDNSResponse};
    return {p, h, nullptr};
}

// Go's Resolver.dial.
std::pair<ptr<Conn>, error> resolverDial(Resolver* r, const ptr<context::Context>& ctx, std::string_view network,
                                         std::string_view server) {
    // Calling Dial here is scary -- we have to be sure not to dial a name
    // that will require a DNS lookup, or Dial will call back here to
    // translate it. The DNS config parser has already checked that all
    // the cfg.servers are IP addresses.
    ptr<Conn> c;
    error err;
    if (r && r->Dial) {
        std::tie(c, err) = r->Dial(ctx, network, server);
    } else {
        Dialer d;
        std::tie(c, err) = d.DialContext(ctx, network, server);
    }
    if (err) return {nullptr, mapErr(err)};
    return {c, nullptr};
}

// Go's checkHeader: an error for a response that isn't usable.
std::pair<dnsmessage::RCode, bool> extractExtendedRCode(dnsmessage::Parser p, const dnsmessage::Header& hdr) {
    p.SkipAllAnswers();
    p.SkipAllAuthorities();
    bool hasAdd = false;
    for (;;) {
        auto [ahdr, err] = p.AdditionalHeader();
        if (err) return {hdr.RCode, hasAdd};
        hasAdd = true;
        if (ahdr.Type == dnsmessage::TypeOPT) return {ahdr.ExtendedRCode(hdr.RCode), hasAdd};
        if (p.SkipAdditional()) return {hdr.RCode, hasAdd};
    }
}

error checkHeader(dnsmessage::Parser& p, const dnsmessage::Header& h) {
    auto [rcode, hasAdd] = extractExtendedRCode(p, h);
    if (rcode == dnsmessage::RCodeNameError) return errNoSuchHost;
    auto [ah, err] = p.AnswerHeader();
    if (err && err != dnsmessage::ErrSectionDone) return errCannotUnmarshalDNSMessage;
    // libresolv continues to the next server when it receives an invalid
    // referral response. See golang.org/issue/15434.
    if (rcode == dnsmessage::RCodeSuccess && !h.Authoritative && !h.RecursionAvailable &&
        err == dnsmessage::ErrSectionDone && !hasAdd) {
        return errLameReferral;
    }
    if (rcode != dnsmessage::RCodeSuccess && rcode != dnsmessage::RCodeNameError) {
        // None of the error codes make sense for the query we sent. If we
        // didn't get a name error and we didn't get success, the server
        // is behaving incorrectly or having temporary trouble.
        if (rcode == dnsmessage::RCodeServerFailure) return errServerTemporarilyMisbehaving;
        return errServerMisbehaving;
    }
    return nullptr;
}

error skipToAnswer(dnsmessage::Parser& p, dnsmessage::Type qtype) {
    for (;;) {
        auto [h, err] = p.AnswerHeader();
        if (err == dnsmessage::ErrSectionDone) return errNoSuchHost;
        if (err) return errCannotUnmarshalDNSMessage;
        if (h.Type == qtype) return nullptr;
        if (p.SkipAnswer()) return errCannotUnmarshalDNSMessage;
    }
}

}  // namespace

std::tuple<dnsmessage::Parser, dnsmessage::Header, error> exchange(Resolver* r, const ptr<context::Context>& ctx0,
                                                                   const std::string& server, dnsmessage::Question q,
                                                                   time::Duration timeout, bool useTCP, bool ad) {
    q.Class = dnsmessage::ClassINET;
    auto [req, rerr] = newRequest(q, ad);
    if (rerr) return {{}, {}, errCannotMarshalDNSMessage};
    std::vector<std::string_view> networks = useTCP ? std::vector<std::string_view>{"tcp"}
                                                    : std::vector<std::string_view>{"udp", "tcp"};
    for (auto network : networks) {
        auto [ctx, cancel] = context::WithDeadline(ctx0, time::Now().Add(timeout));
        auto [c, err] = resolverDial(r, ctx, network, server);
        if (err) {
            cancel();
            return {{}, {}, err};
        }
        if (auto d = ctx->Deadline(); d && !d->IsZero()) c->SetDeadline(*d);
        roundTrip rt;
        if (dynamic_cast<PacketConn*>(c.get())) rt = dnsPacketRoundTrip(*c, req.id, q, req.udpReq);
        else rt = dnsStreamRoundTrip(*c, req.id, q, req.tcpReq);
        c->Close();
        cancel();
        auto& [p, h, rterr] = rt;
        if (rterr) return {{}, {}, mapErr(rterr)};
        if (p.SkipQuestion() != dnsmessage::ErrSectionDone) return {{}, {}, errInvalidDNSResponse};
        // RFC 5966 indicates that when a client receives a UDP response
        // with the TC flag set, it should take the TC flag as an
        // indication that it should retry over TCP instead.
        if (h.Truncated && network == "udp") continue;
        return {p, h, nullptr};
    }
    return {{}, {}, errNoAnswerFromDNSServer};
}

std::tuple<dnsmessage::Parser, std::string, error> tryOneName(Resolver* r, const ptr<context::Context>& ctx,
                                                              dnsConfig& cfg, const std::string& name,
                                                              dnsmessage::Type qtype) {
    error lastErr;
    uint32 serverOffset = cfg.serverOffset();
    uint32 sLen = uint32(cfg.servers.size());
    auto [n, nerr] = dnsmessage::NewName(name);
    if (nerr) return {{}, {}, dnsError(errCannotMarshalDNSMessage.Error().view(), name)};
    dnsmessage::Question q;
    q.Name = n;
    q.Type = qtype;
    q.Class = dnsmessage::ClassINET;
    for (int i = 0; i < cfg.attempts; i++) {
        for (uint32 j = 0; j < sLen; j++) {
            const std::string& server = cfg.servers[(serverOffset + j) % sLen];
            auto [p, h, err] = exchange(r, ctx, server, q, cfg.timeout, cfg.useTCP, cfg.trustAD);
            if (err) {
                auto dnsErr = newDNSError(err, name, server);
                // Set IsTemporary for socket-level errors. Note that this
                // flag may also be used to indicate a SERVFAIL response.
                if (dynamic_cast<const OpError*>(err.get())) dnsErr->IsTemporary = true;
                lastErr = dnsErr;
                continue;
            }
            if (error herr = checkHeader(p, h)) {
                if (herr == errNoSuchHost) return {p, server, newDNSError(errNoSuchHost, name, server)};
                lastErr = newDNSError(herr, name, server);
                continue;
            }
            if (error serr = skipToAnswer(p, qtype)) {
                if (serr == errNoSuchHost) return {p, server, newDNSError(errNoSuchHost, name, server)};
                lastErr = newDNSError(serr, name, server);
                continue;
            }
            return {p, server, nullptr};
        }
    }
    return {{}, {}, lastErr};
}

std::tuple<dnsmessage::Parser, std::string, error> resolverLookup(Resolver* r, const ptr<context::Context>& ctx,
                                                                  std::string_view name, dnsmessage::Type qtype,
                                                                  std::shared_ptr<dnsConfig> conf) {
    if (!isDomainName(name)) {
        // We used to use "invalid domain name" as the error, but that is
        // a detail of the specific lookup mechanism.
        return {{}, {}, newDNSError(errNoSuchHost, name, "")};
    }
    if (!conf) conf = getSystemDNSConfig();
    dnsmessage::Parser p;
    std::string server;
    error err;
    for (auto& fqdn : conf->nameList(name)) {
        std::tie(p, server, err) = tryOneName(r, ctx, *conf, fqdn, qtype);
        if (!err) break;
        // If we hit a temporary error with StrictErrors enabled, stop
        // immediately instead of trying more names.
        if (isTemporaryError(err) && r && r->StrictErrors) break;
    }
    if (!err) return {p, server, nullptr};
    if (auto* d = const_cast<DNSError*>(dynamic_cast<const DNSError*>(err.get()))) {
        // Show original name passed to lookup, not suffixed one. In
        // general we might have tried many suffixes; showing just one is
        // misleading.
        d->Name = string(name);
    }
    return {{}, {}, err};
}

std::pair<std::vector<std::string>, error> goLookupHostOrder(Resolver* r, const ptr<context::Context>& ctx,
                                                             std::string_view name, hostLookupOrder order,
                                                             std::shared_ptr<dnsConfig> conf) {
    if (order == hostLookupFilesDNS || order == hostLookupFiles) {
        // Use entries from /etc/hosts if they match.
        auto [addrs, canonical] = lookupStaticHost(name);
        if (!addrs.empty()) return {addrs, nullptr};
        if (order == hostLookupFiles) return {{}, newDNSError(errNoSuchHost, name, "")};
    }
    auto [ips, cname, err] = goLookupIPCNAMEOrder(r, ctx, "ip", name, order, conf);
    if (err) return {{}, err};
    std::vector<std::string> addrs;
    for (auto& ip : ips) addrs.emplace_back(ip.String().view());
    return {addrs, nullptr};
}

namespace {

// Go's goLookupIPFiles: the addresses of name in /etc/hosts.
std::pair<std::vector<IPAddr>, std::string> goLookupIPFiles(std::string_view name) {
    auto [addr, canonical] = lookupStaticHost(name);
    std::vector<IPAddr> addrs;
    for (auto& haddr0 : addr) {
        std::string_view haddr = haddr0, zone;
        if (auto i = haddr.rfind('%'); i != haddr.npos && i > 0) {
            zone = haddr.substr(i + 1);
            haddr = haddr.substr(0, i);
        }
        if (IP ip = ParseIP(haddr); !ip.empty()) addrs.push_back(IPAddr(ip, string(zone)));
    }
    sortByRFC6724(addrs);
    return {addrs, canonical};
}

}  // namespace

char ipVersion(std::string_view network) {
    if (network.empty()) return 0;
    char n = network.back();
    return n == '4' || n == '6' ? n : 0;
}

std::tuple<std::vector<IPAddr>, dnsmessage::Name, error> goLookupIPCNAMEOrder(Resolver* r,
                                                                             const ptr<context::Context>& ctx,
                                                                             std::string_view network,
                                                                             std::string_view name,
                                                                             hostLookupOrder order,
                                                                             std::shared_ptr<dnsConfig> conf) {
    if (order == hostLookupFilesDNS || order == hostLookupFiles) {
        auto [addrs, canonical] = goLookupIPFiles(name);
        if (!addrs.empty()) {
            auto [cname, err] = dnsmessage::NewName(canonical);
            if (err) return {{}, {}, err};
            return {addrs, cname, nullptr};
        }
        if (order == hostLookupFiles) return {{}, {}, newDNSError(errNoSuchHost, name, "")};
    }
    if (!isDomainName(name)) {
        // See comment in func lookup above about use of errNoSuchHost.
        return {{}, {}, newDNSError(errNoSuchHost, name, "")};
    }
    struct result {
        dnsmessage::Parser p;
        std::string server;
        error err;
    };
    if (!conf) conf = getSystemDNSConfig();
    auto lane = make_chan<result>(1);
    std::vector<dnsmessage::Type> qtypes = {dnsmessage::TypeA, dnsmessage::TypeAAAA};
    if (network == "CNAME") qtypes.push_back(dnsmessage::TypeCNAME);
    switch (ipVersion(network)) {
    case '4': qtypes = {dnsmessage::TypeA}; break;
    case '6': qtypes = {dnsmessage::TypeAAAA}; break;
    }
    // The queries of a name run in parallel unless single-request says
    // otherwise; the results come back through lane in any order.
    auto queryFn = [&](const std::string& fqdn, dnsmessage::Type qtype) {
        if (conf->singleRequest) return;
        // The results are all received before this function returns, so
        // r outlives the fibers.
        go([ctx, conf, fqdn, qtype, lane, r] {
            auto [p, server, err] = tryOneName(r, ctx, *conf, fqdn, qtype);
            lane.send(result{p, server, err});
        });
    };
    auto responseFn = [&](const std::string& fqdn, dnsmessage::Type qtype) -> result {
        if (conf->singleRequest) {
            auto [p, server, err] = tryOneName(r, ctx, *conf, fqdn, qtype);
            return {p, server, err};
        }
        return lane.recv();
    };
    std::vector<IPAddr> addrs;
    dnsmessage::Name cname;
    error lastErr;
    auto unmarshalErr = [&](const std::string& server) {
        lastErr = dnsError(errCannotUnmarshalDNSMessage.Error().view(), name, server);
    };
    for (auto& fqdn : conf->nameList(name)) {
        for (auto qtype : qtypes) queryFn(fqdn, qtype);
        bool hitStrictError = false;
        for (auto qtype : qtypes) {
            result res = responseFn(fqdn, qtype);
            if (res.err) {
                if (isTemporaryError(res.err) && r && r->StrictErrors) {
                    // This error will abort the nameList loop.
                    hitStrictError = true;
                    lastErr = res.err;
                } else if (!lastErr || fqdn == std::string(name) + ".") {
                    // Prefer error for original name.
                    lastErr = res.err;
                }
                continue;
            }
            // Presotto says it's okay to assume that servers listed in
            // /etc/resolv.conf are recursive resolvers.
            for (;;) {
                auto [h, err] = res.p.AnswerHeader();
                if (err && err != dnsmessage::ErrSectionDone) unmarshalErr(res.server);
                if (err) break;
                bool stop = false;
                switch (h.Type) {
                case dnsmessage::TypeA: {
                    auto [a, aerr] = res.p.AResource();
                    if (aerr) {
                        unmarshalErr(res.server);
                        stop = true;
                        break;
                    }
                    addrs.push_back(IPAddr(IP(slice<byte>::of(a.A))));
                    if (cname.Length == 0 && h.Name.Length != 0) cname = h.Name;
                    break;
                }
                case dnsmessage::TypeAAAA: {
                    auto [aaaa, aerr] = res.p.AAAAResource();
                    if (aerr) {
                        unmarshalErr(res.server);
                        stop = true;
                        break;
                    }
                    addrs.push_back(IPAddr(IP(slice<byte>::of(aaaa.AAAA))));
                    if (cname.Length == 0 && h.Name.Length != 0) cname = h.Name;
                    break;
                }
                case dnsmessage::TypeCNAME: {
                    auto [c, cerr] = res.p.CNAMEResource();
                    if (cerr) {
                        unmarshalErr(res.server);
                        stop = true;
                        break;
                    }
                    if (cname.Length == 0 && c.CNAME.Length > 0) cname = c.CNAME;
                    break;
                }
                default:
                    if (res.p.SkipAnswer()) {
                        unmarshalErr(res.server);
                        stop = true;
                    }
                    break;
                }
                if (stop) break;
            }
        }
        if (hitStrictError) {
            // If either family hit an error with StrictErrors enabled,
            // discard all addresses. This ensures that network flakiness
            // cannot turn a dualstack hostname IPv4/IPv6-only.
            addrs.clear();
            break;
        }
        if (!addrs.empty() || (network == "CNAME" && cname.Length > 0)) break;
    }
    if (auto* d = const_cast<DNSError*>(dynamic_cast<const DNSError*>(lastErr.get()))) {
        // Show original name passed to lookup, not suffixed one.
        d->Name = string(name);
    }
    sortByRFC6724(addrs);
    if (addrs.empty() && !(network == "CNAME" && cname.Length > 0)) {
        if (order == hostLookupDNSFiles) {
            auto [faddrs, canonical] = goLookupIPFiles(name);
            if (!faddrs.empty()) {
                auto [c, err] = dnsmessage::NewName(canonical);
                if (err) return {{}, {}, err};
                return {faddrs, c, nullptr};
            }
        }
        if (lastErr) return {{}, {}, lastErr};
    }
    return {addrs, cname, nullptr};
}

std::pair<std::vector<std::string>, error> goLookupPTR(Resolver* r, const ptr<context::Context>& ctx,
                                                       std::string_view addr, hostLookupOrder order,
                                                       std::shared_ptr<dnsConfig> conf) {
    if (order == hostLookupFiles || order == hostLookupFilesDNS) {
        auto names = lookupStaticAddr(addr);
        if (!names.empty()) return {names, nullptr};
        if (order == hostLookupFiles) return {{}, newDNSError(errNoSuchHost, addr, "")};
    }
    auto [arpa, aerr] = reverseaddr(addr);
    if (aerr) return {{}, aerr};
    auto [p, server, err] = resolverLookup(r, ctx, arpa, dnsmessage::TypePTR, conf);
    if (err) {
        auto* d = errors::As<DNSError>(err);
        if (d && d->IsNotFound && order == hostLookupDNSFiles) {
            auto names = lookupStaticAddr(addr);
            if (!names.empty()) return {names, nullptr};
        }
        return {{}, err};
    }
    std::vector<std::string> ptrs;
    for (;;) {
        auto [h, herr] = p.AnswerHeader();
        if (herr == dnsmessage::ErrSectionDone) break;
        if (herr) return {{}, dnsError(errCannotUnmarshalDNSMessage.Error().view(), addr, server)};
        if (h.Type != dnsmessage::TypePTR) {
            if (p.SkipAnswer()) return {{}, dnsError(errCannotUnmarshalDNSMessage.Error().view(), addr, server)};
            continue;
        }
        auto [ptr_, perr] = p.PTRResource();
        if (perr) return {{}, dnsError(errCannotUnmarshalDNSMessage.Error().view(), addr, server)};
        ptrs.push_back(ptr_.PTR.String());
    }
    return {ptrs, nullptr};
}

// ---------------------------------------------------------------------------
// internal/singleflight

namespace detail {

chan<lookupResult> lookupGroup::DoChan(const std::string& key,
                                         std::function<std::pair<std::vector<IPAddr>, error>()> fn) {
    auto ch = make_chan<lookupResult>(1);
    std::unique_lock l(mu);
    if (auto it = m.find(key); it != m.end()) {
        it->second->dups++;
        it->second->chans.push_back(ch);
        return ch;
    }
    auto c = std::make_shared<call>();
    c->chans.push_back(ch);
    m[key] = c;
    l.unlock();
    auto self = shared_from_this();
    go([self, c, key, fn = std::move(fn)] {
        auto& mu = self->mu;
        auto& m = self->m;
        std::tie(c->val, c->err) = fn();
        std::lock_guard lg(mu);
        if (auto it = m.find(key); it != m.end() && it->second == c) m.erase(it);
        for (auto& rc : c->chans) rc.send(lookupResult{c->val, c->err, c->dups > 0});
    });
    return ch;
}

bool lookupGroup::ForgetUnshared(const std::string& key) {
    std::lock_guard l(mu);
    auto it = m.find(key);
    if (it == m.end()) return true;
    if (it->second->dups == 0) {
        m.erase(it);
        return true;
    }
    return false;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// lookup.go

namespace {

// Go's onlyValuesCtx: the values of the lookup's context while it isn't
// done, without its deadline and cancellation, for lookups shared with
// other callers.
struct onlyValuesCtx final : context::Context {
    ptr<context::Context> base = context::Background();
    ptr<context::Context> lookupValues;
    explicit onlyValuesCtx(ptr<context::Context> v) : lookupValues(std::move(v)) {}
    chan<empty> Done() const override { return base->Done(); }
    error Err() const override { return base->Err(); }
    ptr<object> Value(const void* key) const override {
        if (lookupValues->Err()) return nullptr;
        return lookupValues->Value(key);
    }
};

Resolver* resolver(Resolver* r) { return r ? r : DefaultResolver.get(); }

}  // namespace

std::pair<std::vector<IPAddr>, error> lookupIPAddr(Resolver* r0, const ptr<context::Context>& ctx,
                                                   std::string_view network, std::string_view host) {
    Resolver* r = resolver(r0);
    // Make sure that no matter what we do later, host=="" is rejected.
    if (host.empty()) return {{}, newDNSError(errNoSuchHost, host, "")};
    if (auto [ip, err] = netip::ParseAddr(host); !err) {
        return {{IPAddr(IP(ip.AsSlice()).To16(), ip.Zone())}, nullptr};
    }
    // We don't want a cancellation of ctx to affect the lookupGroup
    // operation. Otherwise if our context gets canceled it might cause an
    // error to be returned to a lookup using a completely different
    // context. However we need to preserve only the values in context.
    auto [lookupGroupCtx, lookupGroupCancel] =
        context::WithCancel(make_ptr<onlyValuesCtx>(ctx));
    std::string lookupKey = std::string(network) + '\0' + std::string(host);
    // The lookup may outlive this call (when ctx is canceled while
    // others share it), and r with it: it works with a copy of r's
    // settings.
    auto keep = make_ptr<Resolver>();
    keep->PreferGo = r->PreferGo;
    keep->StrictErrors = r->StrictErrors;
    keep->Dial = r->Dial;
    auto group = r->lookupGroupPtr();
    std::string net(network), h(host);
    auto gctx = lookupGroupCtx;
    auto ch = group->DoChan(lookupKey, [keep, gctx, net, h]() -> std::pair<std::vector<IPAddr>, error> {
        auto [order, conf] = hostLookupOrderFor(h);
        auto [ips, cname, err] = goLookupIPCNAMEOrder(keep.get(), gctx, net, h, order, conf);
        return {ips, err};
    });
    std::pair<std::vector<IPAddr>, error> ret;
    select(recv(ctx->Done(),
                [&](empty) {
                    // Our context was canceled. If we are the only
                    // goroutine looking up this key, then drop the key
                    // from the lookupGroup and cancel the lookup. If there
                    // are other goroutines looking up this key, let the
                    // lookup continue uncanceled, and let later lookups
                    // with the same key share the result.
                    auto cancel = lookupGroupCancel;
                    if (group->ForgetUnshared(lookupKey)) {
                        cancel();
                        go([ch] { ch.recv(); });
                    } else {
                        go([ch, cancel] {
                            ch.recv();
                            cancel();
                        });
                    }
                    ret = {{}, newDNSError(mapErr(ctx->Err()), host, "")};
                }),
           recv(ch, [&](detail::lookupResult res) {
               lookupGroupCancel();
               error err = res.err;
               if (err && !dynamic_cast<const DNSError*>(err.get())) err = newDNSError(mapErr(err), host, "");
               if (err) ret = {{}, err};
               else ret = {res.val, nullptr};
           }));
    return ret;
}

std::pair<std::vector<ptr<Addr>>, error> internetAddrList(Resolver* r, const ptr<context::Context>& ctx,
                                                          std::string_view net, std::string_view addr) {
    string host, port;
    int portnum = 0;
    bool udp = false, ip = false;
    if (net == "tcp" || net == "tcp4" || net == "tcp6" || net == "udp" || net == "udp4" || net == "udp6") {
        udp = net.starts_with("udp");
        if (!addr.empty()) {
            error err;
            std::tie(host, port, err) = SplitHostPort(addr);
            if (err) return {{}, err};
            std::tie(portnum, err) = resolver(r)->LookupPort(ctx, net, port.view());
            if (err) return {{}, err};
        }
    } else if (net == "ip" || net == "ip4" || net == "ip6") {
        ip = true;
        if (!addr.empty()) host = string(addr);
    } else {
        return {{}, make_ptr<UnknownNetworkError>(string(net))};
    }
    auto inetaddr = [&](const IPAddr& a) -> ptr<Addr> {
        if (ip) return make_ptr<IPAddr>(a.IP, a.Zone);
        if (udp) return make_ptr<UDPAddr>(a.IP, portnum, a.Zone);
        return make_ptr<TCPAddr>(a.IP, portnum, a.Zone);
    };
    if (host.empty()) return {{inetaddr(IPAddr{})}, nullptr};

    // Try as a literal IP address, then as a DNS name.
    auto [ips, err] = lookupIPAddr(r, ctx, net, host.view());
    if (err) return {{}, err};
    // Issue 18806: if the machine has halfway configured IPv6 such that
    // it can bind on "::" (IPv6unspecified) but not connect back to that
    // same address, fall back to dialing 0.0.0.0.
    if (ips.size() == 1 && ips[0].IP.Equal(IPv6unspecified)) ips.push_back(IPAddr(IPv4zero));

    std::vector<ptr<Addr>> addrs;
    for (auto& a : ips) {
        if (net.back() == '4' && a.IP.To4().empty()) continue;                                // ipv4only
        if (net.back() == '6' && (a.IP.size() != IPv6len || !a.IP.To4().empty())) continue;  // ipv6only
        addrs.push_back(inetaddr(a));
    }
    if (addrs.empty()) return {{}, make_ptr<AddrError>(string::fromStatic("no suitable address found"), host)};
    return {addrs, nullptr};
}

const ptr<Resolver> DefaultResolver = make_ptr<Resolver>();

Resolver::Resolver() : group_(std::make_shared<detail::lookupGroup>()) {}
Resolver::~Resolver() = default;

std::pair<std::vector<string>, error> Resolver::LookupHost(const ptr<context::Context>& ctx, std::string_view host) {
    // Make sure that no matter what we do later, host=="" is rejected.
    if (host.empty()) return {{}, newDNSError(errNoSuchHost, host, "")};
    if (auto [ip, err] = netip::ParseAddr(host); !err) return {{string(host)}, nullptr};
    auto [order, conf] = hostLookupOrderFor(host);
    auto [addrs, err] = goLookupHostOrder(this, ctx, host, order, conf);
    if (err) return {{}, err};
    return {std::vector<string>(addrs.begin(), addrs.end()), nullptr};
}

std::pair<std::vector<IPAddr>, error> Resolver::LookupIPAddr(const ptr<context::Context>& ctx, std::string_view host) {
    return lookupIPAddr(this, ctx, "ip", host);
}

std::pair<std::vector<IP>, error> Resolver::LookupIP(const ptr<context::Context>& ctx, std::string_view network,
                                                     std::string_view host) {
    auto [afnet, proto, err] = parseNetwork(ctx, network, false);
    if (err) return {{}, err};
    if (afnet != "ip" && afnet != "ip4" && afnet != "ip6") return {{}, make_ptr<UnknownNetworkError>(string(network))};
    if (host.empty()) return {{}, newDNSError(errNoSuchHost, host, "")};
    auto [addrs, aerr] = internetAddrList(this, ctx, afnet.view(), host);
    if (aerr) return {{}, aerr};
    std::vector<IP> ips;
    for (auto& a : addrs) ips.push_back(static_cast<IPAddr*>(a.get())->IP);
    return {ips, nullptr};
}

std::pair<std::vector<netip::Addr>, error> Resolver::LookupNetIP(const ptr<context::Context>& ctx,
                                                                std::string_view network, std::string_view host) {
    // TODO(bradfitz): make this efficient, making the internal net
    // package type throughout be netip.Addr and only converting to the
    // net.IP slice form when necessary. As Go has it.
    auto [ips, err] = LookupIP(ctx, network, host);
    if (err) return {{}, err};
    std::vector<netip::Addr> ret;
    for (auto& ip : ips) {
        if (auto [a, ok] = netip::AddrFromSlice(ip); ok) ret.push_back(a);
    }
    return {ret, nullptr};
}

std::pair<int, error> Resolver::LookupPort(const ptr<context::Context>&, std::string_view network,
                                           std::string_view service) {
    return net::LookupPort(network, service);
}

std::pair<string, error> Resolver::LookupCNAME(const ptr<context::Context>& ctx, std::string_view host) {
    auto [order, conf] = hostLookupOrderFor(host);
    auto [ips, cname, err] = goLookupIPCNAMEOrder(this, ctx, "CNAME", host, order, conf);
    if (err) return {{}, err};
    std::string c = cname.String();
    if (!isDomainName(c)) return {{}, dnsError(errMalformedDNSRecordsDetail, host)};
    return {string(c), nullptr};
}

namespace {

// The records of name's qtype answers, through each.
template <typename F>
error forAnswers(Resolver* r, const ptr<context::Context>& ctx, std::string_view lookupName, std::string_view errName,
                 dnsmessage::Type qtype, F each) {
    auto [p, server, err] = resolverLookup(r, ctx, lookupName, qtype, nullptr);
    if (err) return err;
    for (;;) {
        auto [h, herr] = p.AnswerHeader();
        if (herr == dnsmessage::ErrSectionDone) break;
        if (herr) return dnsError("cannot unmarshal DNS message", errName, server);
        if (h.Type != qtype) {
            if (p.SkipAnswer()) return dnsError("cannot unmarshal DNS message", errName, server);
            continue;
        }
        if (!each(p, h)) return dnsError("cannot unmarshal DNS message", errName, server);
    }
    return nullptr;
}

}  // namespace

std::tuple<string, std::vector<ptr<SRV>>, error> Resolver::LookupSRV(const ptr<context::Context>& ctx,
                                                                     std::string_view service, std::string_view proto,
                                                                     std::string_view name) {
    std::string target = service.empty() && proto.empty()
                             ? std::string(name)
                             : "_" + std::string(service) + "._" + std::string(proto) + "." + std::string(name);
    dnsmessage::Name cname;
    std::vector<ptr<SRV>> srvs;
    error err = forAnswers(this, ctx, target, name, dnsmessage::TypeSRV,
                           [&](dnsmessage::Parser& p, const dnsmessage::ResourceHeader& h) {
                               if (cname.Length == 0 && h.Name.Length != 0) cname = h.Name;
                               auto [srv, e] = p.SRVResource();
                               if (e) return false;
                               auto s = make_ptr<SRV>();
                               s->Target = string(srv.Target.String());
                               s->Port = srv.Port;
                               s->Priority = srv.Priority;
                               s->Weight = srv.Weight;
                               srvs.push_back(s);
                               return true;
                           });
    if (err) return {{}, {}, err};
    sortSRV(srvs);
    std::string c = cname.String();
    if (!c.empty() && !isDomainName(c)) return {{}, {}, dnsError("SRV header name is invalid", name)};
    std::vector<ptr<SRV>> filtered;
    for (auto& a : srvs) {
        if (isDomainName(a->Target.view())) filtered.push_back(a);
    }
    if (filtered.size() != srvs.size()) return {string(c), filtered, dnsError(errMalformedDNSRecordsDetail, name)};
    return {string(c), filtered, nullptr};
}

std::pair<std::vector<ptr<MX>>, error> Resolver::LookupMX(const ptr<context::Context>& ctx, std::string_view name) {
    std::vector<ptr<MX>> mxs;
    error err = forAnswers(this, ctx, name, name, dnsmessage::TypeMX,
                           [&](dnsmessage::Parser& p, const dnsmessage::ResourceHeader&) {
                               auto [mx, e] = p.MXResource();
                               if (e) return false;
                               auto m = make_ptr<MX>();
                               m->Host = string(mx.MX.String());
                               m->Pref = mx.Pref;
                               mxs.push_back(m);
                               return true;
                           });
    if (err) return {{}, err};
    sortMX(mxs);
    std::vector<ptr<MX>> filtered;
    for (auto& mx : mxs) {
        // Bypass the hostname validity check for targets which contain
        // an IP address, as some local mail servers set up their MX that
        // way.
        if (!isDomainName(mx->Host.view())) {
            std::string_view h = mx->Host.view();
            if (!h.empty() && h.back() == '.') h.remove_suffix(1);
            auto [ip, perr] = netip::ParseAddr(h);
            if (perr || !ip.Zone().empty()) continue;
        }
        filtered.push_back(mx);
    }
    if (filtered.size() != mxs.size()) return {filtered, dnsError(errMalformedDNSRecordsDetail, name)};
    return {filtered, nullptr};
}

std::pair<std::vector<ptr<NS>>, error> Resolver::LookupNS(const ptr<context::Context>& ctx, std::string_view name) {
    std::vector<ptr<NS>> nss;
    error err = forAnswers(this, ctx, name, name, dnsmessage::TypeNS,
                           [&](dnsmessage::Parser& p, const dnsmessage::ResourceHeader&) {
                               auto [ns, e] = p.NSResource();
                               if (e) return false;
                               auto n = make_ptr<NS>();
                               n->Host = string(ns.NS.String());
                               nss.push_back(n);
                               return true;
                           });
    if (err) return {{}, err};
    std::vector<ptr<NS>> filtered;
    for (auto& ns : nss) {
        if (isDomainName(ns->Host.view())) filtered.push_back(ns);
    }
    if (filtered.size() != nss.size()) return {filtered, dnsError(errMalformedDNSRecordsDetail, name)};
    return {filtered, nullptr};
}

std::pair<std::vector<string>, error> Resolver::LookupTXT(const ptr<context::Context>& ctx, std::string_view name) {
    std::vector<string> txts;
    error err = forAnswers(this, ctx, name, name, dnsmessage::TypeTXT,
                           [&](dnsmessage::Parser& p, const dnsmessage::ResourceHeader&) {
                               auto [txt, e] = p.TXTResource();
                               if (e) return false;
                               // Multiple strings in one TXT record need
                               // to be concatenated without separator to
                               // be consistent with previous Go resolver.
                               std::string joined;
                               for (auto& s : txt.TXT) joined += s;
                               txts.push_back(string(joined));
                               return true;
                           });
    if (err) return {{}, err};
    return {txts, nullptr};
}

std::pair<std::vector<string>, error> Resolver::LookupAddr(const ptr<context::Context>& ctx, std::string_view addr) {
    auto [order, conf] = hostLookupOrderFor("");
    auto [names, err] = goLookupPTR(this, ctx, addr, order, conf);
    if (err) return {{}, err};
    std::vector<string> filtered;
    for (auto& name : names) {
        if (isDomainName(name)) filtered.push_back(string(name));
    }
    if (filtered.size() != names.size()) return {filtered, dnsError(errMalformedDNSRecordsDetail, addr)};
    return {filtered, nullptr};
}

std::pair<std::vector<string>, error> LookupHost(std::string_view host) {
    return DefaultResolver->LookupHost(context::Background(), host);
}

std::pair<std::vector<IP>, error> LookupIP(std::string_view host) {
    auto [addrs, err] = DefaultResolver->LookupIPAddr(context::Background(), host);
    if (err) return {{}, err};
    std::vector<IP> ips;
    for (auto& a : addrs) ips.push_back(a.IP);
    return {ips, nullptr};
}

std::pair<string, error> LookupCNAME(std::string_view host) {
    return DefaultResolver->LookupCNAME(context::Background(), host);
}

std::tuple<string, std::vector<ptr<SRV>>, error> LookupSRV(std::string_view service, std::string_view proto,
                                                           std::string_view name) {
    return DefaultResolver->LookupSRV(context::Background(), service, proto, name);
}

std::pair<std::vector<ptr<MX>>, error> LookupMX(std::string_view name) {
    return DefaultResolver->LookupMX(context::Background(), name);
}

std::pair<std::vector<ptr<NS>>, error> LookupNS(std::string_view name) {
    return DefaultResolver->LookupNS(context::Background(), name);
}

std::pair<std::vector<string>, error> LookupTXT(std::string_view name) {
    return DefaultResolver->LookupTXT(context::Background(), name);
}

std::pair<std::vector<string>, error> LookupAddr(std::string_view addr) {
    return DefaultResolver->LookupAddr(context::Background(), addr);
}

string IPAddr::String() const {
    string ip = IP.empty() ? string() : IP.String();
    if (!Zone.empty()) return ip + "%" + Zone;
    return ip;
}

std::pair<ptr<IPAddr>, error> ResolveIPAddr(std::string_view network, std::string_view address) {
    if (network.empty()) network = "ip";  // a hint wildcard for Go 1.0 undocumented behavior
    auto [afnet, proto, err] = parseNetwork(context::Background(), network, false);
    if (err) return {nullptr, err};
    if (afnet != "ip" && afnet != "ip4" && afnet != "ip6") return {nullptr, make_ptr<UnknownNetworkError>(string(network))};
    auto [addrs, aerr] = internetAddrList(nullptr, context::Background(), afnet.view(), address);
    if (aerr) return {nullptr, aerr};
    ptr<Addr> a = forResolve(addrs, network, address);
    return {ptr<IPAddr>(static_cast<IPAddr*>(a.get())), nullptr};
}

}  // namespace gopp::net
