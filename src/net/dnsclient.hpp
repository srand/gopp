// Go's resolver, for lookup.cpp and the tests: dnsclient.go,
// dnsclient_unix.go, dnsconfig.go, dnsconfig_unix.go, hosts.go, nss.go,
// conf.go, addrselect.go and internal/singleflight, as Go 1.27.3 has them.
#pragma once

#include "dnsmessage.hpp"

#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/net.hpp>
#include <gopp/sync.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gopp::net {

// ---------------------------------------------------------------------------
// Errors

extern const error errNoSuchHost;  // a notFoundError: "no such host"
extern const error errUnknownPort;
extern const error errLameReferral;
extern const error errCannotUnmarshalDNSMessage;
extern const error errCannotMarshalDNSMessage;
extern const error errServerMisbehaving;
extern const error errInvalidDNSResponse;
extern const error errNoAnswerFromDNSServer;
extern const error errServerTemporarilyMisbehaving;  // a temporaryError
extern const char* const errMalformedDNSRecordsDetail;

// Go's newDNSError: a DNSError for err, with its Timeout, Temporary and
// not-found flags, wrapping it if it's a context's error.
ptr<DNSError> newDNSError(const error& err, std::string_view name, std::string_view server);

// Whether err is a net.Error whose Temporary() is true.
bool isTemporaryError(const error& err);

// ---------------------------------------------------------------------------
// parse.go

std::vector<std::string> getFields(std::string_view s);
std::tuple<int, std::size_t, bool> dtoi(std::string_view s);
bool stringsHasSuffixFold(std::string_view s, std::string_view suffix);
bool stringsEqualFold(std::string_view a, std::string_view b);
std::string lowerASCII(std::string_view s);

// ---------------------------------------------------------------------------
// dnsclient.go

int randInt();
int randIntn(int n);
// The reverse DNS name of addr (".in-addr.arpa." or ".ip6.arpa.").
std::pair<std::string, error> reverseaddr(std::string_view addr);
bool equalASCIIName(const dnsmessage::Name& x, const dnsmessage::Name& y);
// Whether s is a legal domain name (RFC 1035 with Go's relaxations:
// underscores, and labels of only digits).
bool isDomainName(std::string_view s);
// s rooted if it has a dot in it.
std::string absDomainName(std::string_view s);
void sortSRV(std::vector<ptr<SRV>>& addrs);
// Go's byPriorityWeight.shuffleByWeight.
void shuffleByWeight(ptr<SRV>* addrs, std::size_t n);
void sortMX(std::vector<ptr<MX>>& mxs);

// ---------------------------------------------------------------------------
// dnsconfig.go and dnsconfig_unix.go

// A parsed resolv.conf.
struct dnsConfig {
    std::vector<std::string> servers;  // server addresses (host:port)
    std::vector<std::string> search;   // rooted suffixes to append to local name
    int ndots = 1;                     // dots in a name to trigger an absolute lookup
    time::Duration timeout = 5 * time::Second;  // per query, including retries
    int attempts = 2;                  // lost packets before giving up on a server
    bool rotate = false;               // round robin among servers
    bool unknownOpt = false;           // anything unknown was encountered
    std::vector<std::string> lookup;   // OpenBSD's lookup order
    error err;                         // any error opening resolv.conf
    time::Time mtime;                  // time of resolv.conf modification
    std::atomic<uint32> soffset{0};    // for serverOffset
    bool singleRequest = false;        // sequential A and AAAA queries
    bool useTCP = false;               // TCP only
    bool trustAD = false;              // set the AD flag in queries
    bool noReload = false;             // don't check the file for updates
    bool defaultNS = false;            // servers are defaultNS

    uint32 serverOffset();
    // The names to try for name, as the search list and ndots say.
    std::vector<std::string> nameList(std::string_view name) const;
};

extern const std::vector<std::string> defaultNS;
// os.Hostname, replaceable by tests.
extern std::function<std::pair<string, error>()> getHostname;
std::shared_ptr<dnsConfig> dnsReadConfig(const std::string& filename);
std::vector<std::string> dnsDefaultSearch();
std::string ensureRooted(std::string_view s);
// Whether name must not go to DNS (".onion", RFC 7686).
bool avoidDNS(std::string_view name);

// The system configuration, reread when /etc/resolv.conf changes.
std::shared_ptr<dnsConfig> getSystemDNSConfig();
// Replaces the configuration with path's and stops rechecks, for tests;
// an empty path restores /etc/resolv.conf.
void resolvConfForceUpdate(const std::string& path);

// ---------------------------------------------------------------------------
// hosts.go

// The hosts file (for tests).
extern std::string hostsFilePath;
// The addresses of host in the hosts file, and its canonical name.
std::pair<std::vector<std::string>, std::string> lookupStaticHost(std::string_view host);
// The names of addr in the hosts file.
std::vector<std::string> lookupStaticAddr(std::string_view addr);

// ---------------------------------------------------------------------------
// nss.go and conf.go

struct nssCriterion {
    bool negate = false;  // if "!" was present
    std::string status;   // "success", "notfound", ... (lowercase)
    std::string action;   // "return", "continue" (lowercase)
    bool standardStatusAction(bool last) const;
    friend bool operator==(const nssCriterion&, const nssCriterion&) = default;
};

struct nssSource {
    std::string source;  // "files", "dns", "mdns4_minimal", ...
    std::vector<nssCriterion> criteria;
    bool standardCriteria() const;
    friend bool operator==(const nssSource&, const nssSource&) = default;
};

struct nssConf {
    time::Time mtime;
    error err;
    std::map<std::string, std::vector<nssSource>> sources;  // by database ("hosts")
};

std::shared_ptr<nssConf> parseNSSConf(std::string_view text);
std::shared_ptr<nssConf> parseNSSConfFile(const std::string& file);
std::shared_ptr<nssConf> getSystemNSS();
// Replaces the system nsswitch.conf with conf, for tests; nullptr
// restores /etc/nsswitch.conf.
void nssForceUpdate(std::shared_ptr<nssConf> conf);

enum hostLookupOrder {
    hostLookupCgo,       // never: go++ has no cgo resolver
    hostLookupFilesDNS,  // files first
    hostLookupDNSFiles,  // dns first
    hostLookupFiles,     // only files
    hostLookupDNS,       // only DNS
};
std::string hostLookupOrderString(hostLookupOrder o);

// conf.go's lookupOrder for go++: the Go resolver's order as nsswitch.conf
// says, and the DNS configuration.
std::pair<hostLookupOrder, std::shared_ptr<dnsConfig>> hostLookupOrderFor(std::string_view hostname);

// ---------------------------------------------------------------------------
// addrselect.go

enum scope : uint8 {
    scopeInterfaceLocal = 0x1,
    scopeLinkLocal = 0x2,
    scopeAdminLocal = 0x4,
    scopeSiteLocal = 0x5,
    scopeOrgLocal = 0x8,
    scopeGlobal = 0xe,
};

struct ipAttr {
    scope Scope = scope(0);
    uint8 Precedence = 0;
    uint8 Label = 0;
    friend bool operator==(const ipAttr&, const ipAttr&) = default;
};

struct policyTableEntry {
    netip::Prefix Prefix;
    uint8 Precedence = 0;
    uint8 Label = 0;
};

const std::vector<policyTableEntry>& rfc6724policyTable();
policyTableEntry classifyPolicy(netip::Addr ip);
scope classifyScope(netip::Addr ip);
ipAttr ipAttrOf(netip::Addr ip);
int commonPrefixLen(netip::Addr a, IP b);
// Sorts addrs by RFC 6724 destination address selection, with the source
// addresses the kernel would use.
void sortByRFC6724(std::vector<IPAddr>& addrs);
void sortByRFC6724withSrcs(std::vector<IPAddr>& addrs, const std::vector<netip::Addr>& srcs);

// ---------------------------------------------------------------------------
// The lookups

// '4' or '6' for a network that ends in one, 0 otherwise.
char ipVersion(std::string_view network);
// Go's lookupProtocol: an IP protocol's number, from /etc/protocols.
std::pair<int, error> lookupProtocol(const ptr<context::Context>& ctx, std::string_view name);

// Go's lookupIPAddr: an address literal, or the addresses of a name for
// network ("ip", "tcp4", ...), with concurrent lookups of the same name
// shared.
std::pair<std::vector<IPAddr>, error> lookupIPAddr(Resolver* r, const ptr<context::Context>& ctx,
                                                   std::string_view network, std::string_view host);

// Go's Resolver.internetAddrList.
std::pair<std::vector<ptr<Addr>>, error> internetAddrList(Resolver* r, const ptr<context::Context>& ctx,
                                                          std::string_view net, std::string_view addr);

// The DNS client (dnsclient_unix.go).
std::tuple<dnsmessage::Parser, std::string, error> resolverLookup(Resolver* r, const ptr<context::Context>& ctx,
                                                                  std::string_view name, dnsmessage::Type qtype,
                                                                  std::shared_ptr<dnsConfig> conf);
std::tuple<dnsmessage::Parser, std::string, error> tryOneName(Resolver* r, const ptr<context::Context>& ctx,
                                                              dnsConfig& cfg, const std::string& name,
                                                              dnsmessage::Type qtype);
std::tuple<dnsmessage::Parser, dnsmessage::Header, error> exchange(Resolver* r, const ptr<context::Context>& ctx,
                                                                   const std::string& server, dnsmessage::Question q,
                                                                   time::Duration timeout, bool useTCP, bool ad);
std::tuple<std::vector<IPAddr>, dnsmessage::Name, error> goLookupIPCNAMEOrder(Resolver* r,
                                                                             const ptr<context::Context>& ctx,
                                                                             std::string_view network,
                                                                             std::string_view name,
                                                                             hostLookupOrder order,
                                                                             std::shared_ptr<dnsConfig> conf);
std::pair<std::vector<std::string>, error> goLookupHostOrder(Resolver* r, const ptr<context::Context>& ctx,
                                                             std::string_view name, hostLookupOrder order,
                                                             std::shared_ptr<dnsConfig> conf);
std::pair<std::vector<std::string>, error> goLookupPTR(Resolver* r, const ptr<context::Context>& ctx,
                                                       std::string_view addr, hostLookupOrder order,
                                                       std::shared_ptr<dnsConfig> conf);

// internal/singleflight's Group, for lookupIPAddr's results.
namespace detail {
struct lookup_result {
    std::vector<IPAddr> val;
    error err;
    bool shared = false;
};
struct lookup_group : std::enable_shared_from_this<lookup_group> {
    struct call {
        std::vector<IPAddr> val;
        error err;
        int dups = 0;
        std::vector<chan<lookup_result>> chans;
    };
    sync::Mutex mu;
    std::map<std::string, std::shared_ptr<call>> m;

    chan<lookup_result> DoChan(const std::string& key, std::function<std::pair<std::vector<IPAddr>, error>()> fn);
    bool ForgetUnshared(const std::string& key);
};
}  // namespace detail

}  // namespace gopp::net
