// The resolver's configuration, ported from Go 1.27.3's net/parse.go,
// dnsconfig.go, dnsconfig_unix.go, hosts.go, nss.go, conf.go and
// addrselect.go.
#include "dnsclient.hpp"

#include <gopp/os.hpp>
#include <gopp/runtime.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace gopp::net {

// ---------------------------------------------------------------------------
// parse.go

std::vector<std::string> getFields(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    auto isSpace = [](char c) { return c == ' ' || c == '\r' || c == '\t' || c == '\n'; };
    while (i < s.size()) {
        while (i < s.size() && isSpace(s[i])) i++;
        std::size_t j = i;
        while (j < s.size() && !isSpace(s[j])) j++;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::tuple<int, std::size_t, bool> dtoi(std::string_view s) {
    constexpr int big = 0xFFFFFF;
    int n = 0;
    std::size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
        n = n * 10 + (s[i] - '0');
        if (n >= big) return {big, i, false};
    }
    if (i == 0) return {0, 0, false};
    return {n, i, true};
}

namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
}  // namespace

bool stringsEqualFold(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool stringsHasSuffixFold(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && stringsEqualFold(s.substr(s.size() - suffix.size()), suffix);
}

std::string lowerASCII(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}

namespace {

// The lines of a file, as Go's file.readLine gives them; an error if it
// can't be read.
struct fileLines {
    std::vector<std::string> lines;
    time::Time mtime;
    int64 size = 0;
    error err;
};

fileLines readLines(const std::string& name) {
    fileLines f;
    auto [fi, serr] = os::Stat(name);
    if (serr) {
        f.err = serr;
        return f;
    }
    f.mtime = fi->ModTime();
    f.size = fi->Size();
    auto [data, err] = os::ReadFile(name);
    if (err) {
        f.err = err;
        return f;
    }
    std::string_view s = as_string_view(data);
    while (!s.empty()) {
        auto nl = s.find('\n');
        std::string_view line = s.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        f.lines.emplace_back(line);
        if (nl == s.npos) break;
        s.remove_prefix(nl + 1);
    }
    return f;
}

std::string_view trimSpace(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view removeComment(std::string_view line) {
    if (auto i = line.find('#'); i != line.npos) return line.substr(0, i);
    return line;
}

}  // namespace

// ---------------------------------------------------------------------------
// dnsconfig.go and dnsconfig_unix.go

const std::vector<std::string> defaultNS = {"127.0.0.1:53", "[::1]:53"};

std::function<std::pair<string, error>()> getHostname = [] {
    char buf[256];
    if (::gethostname(buf, sizeof buf) < 0) return std::pair<string, error>{{}, errno_error(errno)};
    buf[sizeof buf - 1] = 0;
    return std::pair<string, error>{string(buf), nullptr};
};

uint32 dnsConfig::serverOffset() {
    if (rotate) return soffset.fetch_add(1);  // return 0 to start
    return 0;
}

std::string ensureRooted(std::string_view s) {
    if (!s.empty() && s.back() == '.') return std::string(s);
    return std::string(s) + ".";
}

std::vector<std::string> dnsDefaultSearch() {
    auto [hn, err] = getHostname();
    if (err) return {};  // best effort
    std::string_view h = hn.view();
    if (auto i = h.find('.'); i != h.npos && i < h.size() - 1) return {ensureRooted(h.substr(i + 1))};
    return {};
}

std::shared_ptr<dnsConfig> dnsReadConfig(const std::string& filename) {
    auto conf = std::make_shared<dnsConfig>();
    auto f = readLines(filename);
    if (f.err) {
        conf->servers = defaultNS;
        conf->defaultNS = true;
        conf->search = dnsDefaultSearch();
        conf->err = f.err;
        return conf;
    }
    conf->mtime = f.mtime;
    for (auto& line : f.lines) {
        if (!line.empty() && (line[0] == ';' || line[0] == '#')) continue;  // comment
        auto fs = getFields(line);
        if (fs.empty()) continue;
        if (fs[0] == "nameserver") {  // add one name server
            if (fs.size() > 1 && conf->servers.size() < 3) {  // small, but the standard limit
                // One more check: make sure server name is just an IP address.
                if (!netip::ParseAddr(fs[1]).second) conf->servers.push_back(std::string(JoinHostPort(fs[1], "53").view()));
            }
        } else if (fs[0] == "domain") {  // set search path to just this domain
            if (fs.size() > 1) conf->search = {ensureRooted(fs[1])};
        } else if (fs[0] == "search") {  // set search path to given servers
            conf->search.clear();
            for (std::size_t i = 1; i < fs.size(); i++) {
                std::string name = ensureRooted(fs[i]);
                if (name == ".") continue;
                conf->search.push_back(name);
            }
        } else if (fs[0] == "options") {  // magic options
            for (std::size_t i = 1; i < fs.size(); i++) {
                std::string_view s = fs[i];
                if (s.starts_with("ndots:")) {
                    int n = std::get<0>(dtoi(s.substr(6)));
                    conf->ndots = std::clamp(n, 0, 15);
                } else if (s.starts_with("timeout:")) {
                    int n = std::max(std::get<0>(dtoi(s.substr(8))), 1);
                    conf->timeout = n * time::Second;
                } else if (s.starts_with("attempts:")) {
                    conf->attempts = std::max(std::get<0>(dtoi(s.substr(9))), 1);
                } else if (s == "rotate") {
                    conf->rotate = true;
                } else if (s == "single-request" || s == "single-request-reopen") {
                    // Linux option: http://man7.org/linux/man-pages/man5/resolv.conf.5.html
                    conf->singleRequest = true;
                } else if (s == "use-vc" || s == "usevc" || s == "tcp") {
                    conf->useTCP = true;
                } else if (s == "trust-ad") {
                    conf->trustAD = true;
                } else if (s == "edns0") {
                    // We use EDNS by default.
                } else if (s == "no-reload") {
                    conf->noReload = true;
                } else {
                    conf->unknownOpt = true;
                }
            }
        } else if (fs[0] == "lookup") {
            // OpenBSD option.
            conf->lookup.assign(fs.begin() + 1, fs.end());
        } else {
            conf->unknownOpt = true;
        }
    }
    if (conf->servers.empty()) {
        conf->servers = defaultNS;
        conf->defaultNS = true;
    }
    if (conf->search.empty()) conf->search = dnsDefaultSearch();
    return conf;
}

bool avoidDNS(std::string_view name) {
    if (name.empty()) return true;
    if (name.back() == '.') name.remove_suffix(1);
    return stringsHasSuffixFold(name, ".onion");
}

std::vector<std::string> dnsConfig::nameList(std::string_view name) const {
    // Check name length (see isDomainName).
    bool rooted = !name.empty() && name.back() == '.';
    if (name.size() > 254 || (name.size() == 254 && !rooted)) return {};
    // If name is rooted (trailing dot), try only that name.
    if (rooted) {
        if (avoidDNS(name)) return {};
        return {std::string(name)};
    }
    bool hasNdots = std::count(name.begin(), name.end(), '.') >= ndots;
    std::string n = std::string(name) + ".";
    std::vector<std::string> names;
    // Try the name as an absolute name first if it has enough dots.
    if (hasNdots && !avoidDNS(n)) names.push_back(n);
    // Try suffixes that are not too long (see isDomainName).
    for (auto& suffix : search) {
        std::string fqdn = n + suffix;
        if (!avoidDNS(fqdn) && fqdn.size() <= 254) names.push_back(fqdn);
    }
    // Try unsuffixed, if not tried first above.
    if (!hasNdots && !avoidDNS(n)) names.push_back(n);
    return names;
}

namespace {

// Go's resolverConfig.
struct resolverConfig {
    sync::Mutex mu;  // guards the fields
    std::shared_ptr<dnsConfig> conf;
    time::Time lastChecked;
    std::string path = "/etc/resolv.conf";
    bool rechecks = true;
    std::atomic<bool> checking{false};  // Go's ch semaphore
    sync::Once initOnce;
};

resolverConfig& resolvConf() {
    // Loaded under a sync::Once, not by the static's initializer: loading
    // parks the fiber, and fibers waiting on a static's guard block their
    // threads, and with them the Ps the loading fiber needs to finish.
    static auto* r = new resolverConfig;
    r->initOnce.Do([] {
        r->conf = dnsReadConfig(r->path);
        r->lastChecked = time::Now();
    });
    return *r;
}

}  // namespace

std::shared_ptr<dnsConfig> getSystemDNSConfig() {
    auto& rc = resolvConf();
    std::shared_ptr<dnsConfig> dc;
    {
        std::lock_guard l(rc.mu);
        dc = rc.conf;
    }
    if (dc->noReload) return dc;
    // Ensure only one update at a time checks resolv.conf.
    if (rc.checking.exchange(true)) return dc;
    time::Time now = time::Now();
    std::string path;
    bool recheck;
    {
        std::lock_guard l(rc.mu);
        bool expired = now.After(rc.lastChecked.Add(5 * time::Second));
        recheck = (expired || dc->defaultNS) && rc.rechecks;
        if (recheck) rc.lastChecked = now;
        path = rc.path;
    }
    if (recheck) {
        time::Time mtime;
        if (auto [fi, err] = os::Stat(path); !err) mtime = fi->ModTime();
        if (!mtime.Equal(dc->mtime)) {
            auto conf = dnsReadConfig(path);
            std::lock_guard l(rc.mu);
            rc.conf = conf;
            dc = conf;
        }
    }
    rc.checking = false;
    return dc;
}

void resolvConfForceUpdate(const std::string& path) {
    auto& rc = resolvConf();
    auto conf = dnsReadConfig(path.empty() ? std::string("/etc/resolv.conf") : path);
    std::lock_guard l(rc.mu);
    rc.conf = conf;
    rc.path = path.empty() ? "/etc/resolv.conf" : path;
    rc.rechecks = path.empty();
    rc.lastChecked = time::Now();
}

// ---------------------------------------------------------------------------
// hosts.go

std::string hostsFilePath = "/etc/hosts";

namespace {

constexpr time::Duration cacheMaxAge = 5 * time::Second;

std::string parseLiteralIP(std::string_view addr) {
    auto [ip, err] = netip::ParseAddr(addr);
    if (err) return {};
    return std::string(ip.String().view());
}

struct byName {
    std::vector<std::string> addrs;
    std::string canonicalName;
};

struct hostsCache {
    sync::Mutex mu;
    std::map<std::string, byName> byName_;
    std::map<std::string, std::vector<std::string>> byAddr;
    time::Time expire;
    std::string path;
    time::Time mtime;
    int64 size = 0;
};

hostsCache& hosts() {
    static auto* h = new hostsCache;
    return *h;
}

void readHosts() {
    auto& h = hosts();
    time::Time now = time::Now();
    const std::string& hp = hostsFilePath;
    if (now.Before(h.expire) && h.path == hp && !h.byName_.empty()) return;
    auto f = readLines(hp);
    if (!f.err && h.path == hp && h.mtime.Equal(f.mtime) && h.size == f.size) {
        h.expire = now.Add(cacheMaxAge);
        return;
    }
    // A missing or unreadable file counts as empty; other errors keep
    // the old contents.
    if (f.err && !errors::Is(f.err, fs::ErrNotExist) && !errors::Is(f.err, fs::ErrPermission)) return;
    std::map<std::string, byName> hs;
    std::map<std::string, std::vector<std::string>> is;
    for (auto& line0 : f.lines) {
        std::string_view line = removeComment(line0);
        auto fs = getFields(line);
        if (fs.size() < 2) continue;
        std::string addr = parseLiteralIP(fs[0]);
        if (addr.empty()) continue;
        std::string canonical;
        for (std::size_t i = 1; i < fs.size(); i++) {
            std::string name = absDomainName(fs[i]);
            std::string key = absDomainName(lowerASCII(fs[i]));
            if (i == 1) canonical = key;
            is[addr].push_back(name);
            if (auto it = hs.find(key); it != hs.end()) {
                it->second.addrs.push_back(addr);
                continue;
            }
            hs[key] = byName{{addr}, canonical};
        }
    }
    // Update the data cache.
    h.expire = now.Add(cacheMaxAge);
    h.path = hp;
    h.byName_ = std::move(hs);
    h.byAddr = std::move(is);
    h.mtime = f.mtime;
    h.size = f.size;
}

}  // namespace

std::pair<std::vector<std::string>, std::string> lookupStaticHost(std::string_view host) {
    auto& h = hosts();
    std::lock_guard l(h.mu);
    readHosts();
    if (!h.byName_.empty()) {
        if (auto it = h.byName_.find(absDomainName(lowerASCII(host))); it != h.byName_.end()) {
            return {it->second.addrs, it->second.canonicalName};
        }
    }
    return {};
}

std::vector<std::string> lookupStaticAddr(std::string_view addr0) {
    auto& h = hosts();
    std::lock_guard l(h.mu);
    readHosts();
    std::string addr = parseLiteralIP(addr0);
    if (addr.empty()) return {};
    if (auto it = h.byAddr.find(addr); it != h.byAddr.end()) return it->second;
    return {};
}

// ---------------------------------------------------------------------------
// nss.go

bool nssCriterion::standardStatusAction(bool last) const {
    if (negate) return false;
    std::string def;
    if (status == "success") def = "return";
    else if (status == "notfound" || status == "unavail" || status == "tryagain") def = "continue";
    else return false;  // unknown status
    if (last && action == "return") return true;
    return action == def;
}

bool nssSource::standardCriteria() const {
    for (std::size_t i = 0; i < criteria.size(); i++) {
        if (!criteria[i].standardStatusAction(i == criteria.size() - 1)) return false;
    }
    return true;
}

namespace {

std::pair<std::vector<nssCriterion>, error> parseCriteria(std::string_view x) {
    std::vector<nssCriterion> c;
    for (auto& f0 : getFields(x)) {
        std::string_view f = f0;
        bool not_ = false;
        if (!f.empty() && f[0] == '!') {
            not_ = true;
            f.remove_prefix(1);
        }
        if (f.size() < 3) return {{}, errors::New("criterion too short")};
        auto eq = f.find('=');
        if (eq == f.npos) return {{}, errors::New("criterion lacks equal sign")};
        std::string lf = lowerASCII(f);
        c.push_back({not_, lf.substr(0, eq), lf.substr(eq + 1)});
    }
    return {c, nullptr};
}

}  // namespace

std::shared_ptr<nssConf> parseNSSConf(std::string_view text) {
    auto conf = std::make_shared<nssConf>();
    while (!text.empty()) {
        auto nl = text.find('\n');
        std::string_view line = trimSpace(removeComment(text.substr(0, nl)));
        text = nl == text.npos ? std::string_view() : text.substr(nl + 1);
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon == line.npos) {
            conf->err = errors::New("no colon on line");
            return conf;
        }
        std::string db(trimSpace(line.substr(0, colon)));
        std::string_view srcs = line.substr(colon + 1);
        for (;;) {
            srcs = trimSpace(srcs);
            if (srcs.empty()) break;
            auto sp = srcs.find(' ');
            std::string_view src;
            if (sp == srcs.npos) {
                src = srcs;
                srcs = {};  // done
            } else {
                src = srcs.substr(0, sp);
                srcs = trimSpace(srcs.substr(sp + 1));
            }
            std::vector<nssCriterion> criteria;
            // See if there's a criteria block in brackets.
            if (!srcs.empty() && srcs[0] == '[') {
                auto bclose = srcs.find(']');
                if (bclose == srcs.npos) {
                    conf->err = errors::New("unclosed criterion bracket");
                    return conf;
                }
                error err;
                std::tie(criteria, err) = parseCriteria(srcs.substr(1, bclose - 1));
                if (err) {
                    conf->err = errors::New("invalid criteria: " + std::string(srcs.substr(1, bclose - 1)));
                    return conf;
                }
                srcs = srcs.substr(bclose + 1);
            }
            conf->sources[db].push_back({std::string(src), criteria});
        }
    }
    return conf;
}

std::shared_ptr<nssConf> parseNSSConfFile(const std::string& file) {
    auto [data, err] = os::ReadFile(file);
    if (err) {
        auto c = std::make_shared<nssConf>();
        c->err = err;
        return c;
    }
    auto conf = parseNSSConf(as_string_view(data));
    if (auto [fi, serr] = os::Stat(file); !serr) conf->mtime = fi->ModTime();
    return conf;
}

namespace {

struct nsswitchConfig {
    sync::Mutex mu;
    std::shared_ptr<nssConf> conf;
    time::Time lastChecked;
    bool forced = false;
    std::atomic<bool> checking{false};
    sync::Once initOnce;
};

nsswitchConfig& nssConfig() {
    // Loaded under a sync::Once, not by the static's initializer: loading
    // parks the fiber, and fibers waiting on a static's guard block their
    // threads, and with them the Ps the loading fiber needs to finish.
    static auto* c = new nsswitchConfig;
    c->initOnce.Do([] {
        c->conf = parseNSSConfFile("/etc/nsswitch.conf");
        c->lastChecked = time::Now();
    });
    return *c;
}

}  // namespace

std::shared_ptr<nssConf> getSystemNSS() {
    auto& c = nssConfig();
    if (!c.checking.exchange(true)) {
        time::Time now = time::Now();
        bool check;
        std::shared_ptr<nssConf> cur;
        {
            std::lock_guard l(c.mu);
            check = !c.forced && !c.lastChecked.After(now.Add(-5 * time::Second));
            if (check) c.lastChecked = now;
            cur = c.conf;
        }
        if (check) {
            time::Time mtime;
            if (auto [fi, err] = os::Stat("/etc/nsswitch.conf"); !err) mtime = fi->ModTime();
            if (!mtime.Equal(cur->mtime)) {
                auto conf = parseNSSConfFile("/etc/nsswitch.conf");
                std::lock_guard l(c.mu);
                c.conf = conf;
            }
        }
        c.checking = false;
    }
    std::lock_guard l(c.mu);
    return c.conf;
}

void nssForceUpdate(std::shared_ptr<nssConf> conf) {
    auto& c = nssConfig();
    std::lock_guard l(c.mu);
    c.forced = conf != nullptr;
    c.conf = conf ? conf : parseNSSConfFile("/etc/nsswitch.conf");
}

// ---------------------------------------------------------------------------
// conf.go

std::string hostLookupOrderString(hostLookupOrder o) {
    switch (o) {
    case hostLookupCgo: return "cgo";
    case hostLookupFilesDNS: return "files,dns";
    case hostLookupDNSFiles: return "dns,files";
    case hostLookupFiles: return "files";
    case hostLookupDNS: return "dns";
    }
    return "hostLookupOrder=" + std::to_string(int(o)) + "??";
}

std::pair<hostLookupOrder, std::shared_ptr<dnsConfig>> hostLookupOrderFor(std::string_view hostname) {
    // go++ always uses the Go resolver: Go's lookupOrder with
    // mustUseGoResolver true, so without the branches that hand over to
    // cgo.
    constexpr hostLookupOrder fallbackOrder = hostLookupFilesDNS;
    auto dnsConf = getSystemDNSConfig();
    auto nss = getSystemNSS();
    auto it = nss->sources.find("hosts");
    const std::vector<nssSource> empty;
    const auto& srcs = it == nss->sources.end() ? empty : it->second;
    // If /etc/nsswitch.conf doesn't exist or doesn't specify any sources
    // for "hosts", assume Go's DNS will work fine.
    if (errors::Is(nss->err, fs::ErrNotExist) || (!nss->err && srcs.empty())) return {hostLookupFilesDNS, dnsConf};
    if (nss->err) {
        // We failed to parse or open nsswitch.conf, so we have nothing to
        // base an order on.
        return {fallbackOrder, dnsConf};
    }
    bool hasDNSSource = false, hasDNSSourceChecked = false;
    bool filesSource = false, dnsSource = false;
    std::string first;
    for (std::size_t i = 0; i < srcs.size(); i++) {
        const auto& src = srcs[i];
        if (src.source == "files" || src.source == "dns") {
            if (src.source == "files") {
                filesSource = true;
            } else {
                hasDNSSource = true;
                hasDNSSourceChecked = true;
                dnsSource = true;
            }
            if (first.empty()) first = src.source;
            continue;
        }
        // Some other source (myhostname, mdns...): the Go resolver can't
        // do it. If DNS isn't a source either, DNS is the best guess.
        if (!hasDNSSourceChecked) {
            hasDNSSourceChecked = true;
            for (std::size_t j = i + 1; j < srcs.size(); j++) {
                if (srcs[j].source == "dns") {
                    hasDNSSource = true;
                    break;
                }
            }
        }
        if (!hasDNSSource) {
            dnsSource = true;
            if (first.empty()) first = "dns";
        }
    }
    // Cases where Go can handle it without cgo and C thread overhead, or
    // where the Go resolver has been forced.
    if (filesSource && dnsSource) return {first == "files" ? hostLookupFilesDNS : hostLookupDNSFiles, dnsConf};
    if (filesSource) return {hostLookupFiles, dnsConf};
    if (dnsSource) return {hostLookupDNS, dnsConf};
    // Something weird. Fallback to the default.
    return {fallbackOrder, dnsConf};
}

// ---------------------------------------------------------------------------
// addrselect.go

const std::vector<policyTableEntry>& rfc6724policyTable() {
    static const std::vector<policyTableEntry> t = [] {
        auto a16 = [](std::initializer_list<byte> b) {
            std::array<byte, 16> a{};
            std::copy(b.begin(), b.end(), a.begin());
            return netip::AddrFrom16(a);
        };
        return std::vector<policyTableEntry>{
            // "::1/128"
            {netip::PrefixFrom(a16({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}), 128), 50, 0},
            // "::ffff:0:0/96": IPv4-compatible, etc.
            {netip::PrefixFrom(a16({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}), 96), 35, 4},
            // "::/96"
            {netip::PrefixFrom(a16({}), 96), 1, 3},
            // "2001::/32": Teredo
            {netip::PrefixFrom(a16({0x20, 0x01}), 32), 5, 5},
            // "2002::/16": 6to4
            {netip::PrefixFrom(a16({0x20, 0x02}), 16), 30, 2},
            // "3ffe::/16"
            {netip::PrefixFrom(a16({0x3f, 0xfe}), 16), 1, 12},
            // "fec0::/10"
            {netip::PrefixFrom(a16({0xfe, 0xc0}), 10), 1, 11},
            // "fc00::/7"
            {netip::PrefixFrom(a16({0xfc}), 7), 3, 13},
            // "::/0"
            {netip::PrefixFrom(a16({}), 0), 40, 1},
        };
    }();
    return t;
}

namespace {

struct byRFC6724Info {
    IPAddr addr;
    ipAttr addrAttr;
    netip::Addr src;
    ipAttr srcAttr;
};

// RFC 6724 section 6, destination address selection: -1 prefers a.
int compareByRFC6724(const byRFC6724Info& a, const byRFC6724Info& b) {
    const IP& DA = a.addr.IP;
    const IP& DB = b.addr.IP;
    netip::Addr SourceDA = a.src, SourceDB = b.src;
    const ipAttr &attrDA = a.addrAttr, &attrDB = b.addrAttr;
    const ipAttr &attrSourceDA = a.srcAttr, &attrSourceDB = b.srcAttr;
    constexpr int preferDA = -1, preferDB = 1;
    // Rule 1: Avoid unusable destinations.
    if (!SourceDA.IsValid() && !SourceDB.IsValid()) return 0;
    if (!SourceDB.IsValid()) return preferDA;
    if (!SourceDA.IsValid()) return preferDB;
    // Rule 2: Prefer matching scope.
    if (attrDA.Scope == attrSourceDA.Scope && attrDB.Scope != attrSourceDB.Scope) return preferDA;
    if (attrDA.Scope != attrSourceDA.Scope && attrDB.Scope == attrSourceDB.Scope) return preferDB;
    // Rules 3 and 4 (deprecated, home addresses) don't apply.
    // Rule 5: Prefer matching label.
    if (attrSourceDA.Label == attrDA.Label && attrSourceDB.Label != attrDB.Label) return preferDA;
    if (attrSourceDA.Label != attrDA.Label && attrSourceDB.Label == attrDB.Label) return preferDB;
    // Rule 6: Prefer higher precedence.
    if (attrDA.Precedence > attrDB.Precedence) return preferDA;
    if (attrDA.Precedence < attrDB.Precedence) return preferDB;
    // Rules 7 (native transport) and 8 (smaller scope) — 8:
    if (attrDA.Scope < attrDB.Scope) return preferDA;
    if (attrDA.Scope > attrDB.Scope) return preferDB;
    // Rule 9: Use longest matching prefix, for IPv6 only (RFC 6724 9.2).
    if (DA.To4().empty() && DB.To4().empty()) {
        int commonA = commonPrefixLen(SourceDA, DA), commonB = commonPrefixLen(SourceDB, DB);
        if (commonA > commonB) return preferDA;
        if (commonA < commonB) return preferDB;
    }
    // Rule 10: Otherwise, leave the order unchanged.
    return 0;
}

std::vector<netip::Addr> srcAddrs(const std::vector<IPAddr>& addrs) {
    std::vector<netip::Addr> srcs(addrs.size());
    for (std::size_t i = 0; i < addrs.size(); i++) {
        auto dst = make_ptr<UDPAddr>(addrs[i].IP, 53, addrs[i].Zone);
        auto [c, err] = DialUDP("udp", nullptr, dst);
        if (!err) {
            if (auto* src = dynamic_cast<UDPAddr*>(c->LocalAddr().get())) srcs[i] = netip::AddrFromSlice(src->IP).first;
            c->Close();
        }
    }
    return srcs;
}

}  // namespace

policyTableEntry classifyPolicy(netip::Addr ip) {
    // Prefix.Contains() won't match an IPv6 prefix for an IPv4 address.
    if (ip.Is4()) ip = netip::AddrFrom16(ip.As16());
    for (auto& ent : rfc6724policyTable()) {
        if (ent.Prefix.Contains(ip)) return ent;
    }
    return {};
}

scope classifyScope(netip::Addr ip) {
    if (ip.IsLoopback() || ip.IsLinkLocalUnicast()) return scopeLinkLocal;
    bool ipv6 = ip.Is6() && !ip.Is4In6();
    auto b = ip.As16();
    if (ipv6 && ip.IsMulticast()) return scope(b[1] & 0xf);
    // Site-local addresses are defined in RFC 3513 section 2.5.6 (and
    // deprecated in RFC 3879).
    if (ipv6 && b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return scopeSiteLocal;
    return scopeGlobal;
}

ipAttr ipAttrOf(netip::Addr ip) {
    if (!ip.IsValid()) return {};
    auto match = classifyPolicy(ip);
    return {classifyScope(ip), match.Precedence, match.Label};
}

int commonPrefixLen(netip::Addr a, IP b) {
    if (IP b4 = b.To4(); !b4.empty()) b = b4;
    auto as = a.AsSlice();
    if (as.size() != b.size()) return 0;
    // If IPv6, only up to the prefix (first 64 bits).
    std::size_t n = std::min<std::size_t>(as.size(), 8);
    int cpl = 0;
    for (std::size_t i = 0; i < n; i++) {
        if (as[i] == b[i]) {
            cpl += 8;
            continue;
        }
        int bits = 8;
        byte ab = as[i], bb = b[i];
        for (;;) {
            ab >>= 1;
            bb >>= 1;
            bits--;
            if (ab == bb) return cpl + bits;
        }
    }
    return cpl;
}

void sortByRFC6724withSrcs(std::vector<IPAddr>& addrs, const std::vector<netip::Addr>& srcs) {
    if (addrs.size() != srcs.size()) panic("internal error");
    std::vector<byRFC6724Info> infos(addrs.size());
    for (std::size_t i = 0; i < addrs.size(); i++) {
        auto addrAttrIP = netip::AddrFromSlice(addrs[i].IP).first;
        infos[i] = {addrs[i], ipAttrOf(addrAttrIP), srcs[i], ipAttrOf(srcs[i])};
    }
    std::stable_sort(infos.begin(), infos.end(),
                     [](const byRFC6724Info& a, const byRFC6724Info& b) { return compareByRFC6724(a, b) < 0; });
    for (std::size_t i = 0; i < infos.size(); i++) addrs[i] = infos[i].addr;
}

void sortByRFC6724(std::vector<IPAddr>& addrs) {
    if (addrs.size() < 2) return;
    sortByRFC6724withSrcs(addrs, srcAddrs(addrs));
}

}  // namespace gopp::net
