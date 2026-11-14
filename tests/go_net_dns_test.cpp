// Ported from Go 1.27.3's net/dnsclient_test.go, dnsclient_unix_test.go,
// dnsconfig_unix_test.go, hosts_test.go, nss_test.go and
// addrselect_test.go, with Go's test names and tables, against Go's fake
// DNS server (Resolver.Dial) and testdata files.
//
// Left out: the tests that need the Internet (TestDNSTransportFallback's
// real servers aside, which the fake answers), TestCVE202133195 and
// TestLongDNSNames (long tables of the same checks), TestDNSGoroutineRace
// (a race-detector test), and conf_test.go, whose cases are mostly about
// choosing cgo, which go++ doesn't have.
#include <gopp/context.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include "go_net_testdata.hpp"
#include "net/dnsclient.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::net;
using namespace std::chrono_literals;
namespace dm = gopp::net::dnsmessage;

namespace {

std::string str(const string& s) { return std::string(s.view()); }

// Go's testdata directory, written out once.
const std::string& testdataDir() {
    static std::string dir = [] {
        auto [d, err] = os::MkdirTemp("", "gopp-net-testdata");
        EXPECT_FALSE(err) << err;
        for (auto& [name, data] : go_net_testdata::files()) {
            os::WriteFile(std::string(d.view()) + "/" + name, as_bytes(data), 0644);
        }
        return std::string(d.view());
    }();
    return dir;
}

std::string testdata(std::string_view name) { return testdataDir() + "/" + std::string(name); }

// Restores hostsFilePath at the end of a test.
struct hostsFileGuard {
    std::string orig = hostsFilePath;
    ~hostsFileGuard() { hostsFilePath = orig; }
};

// Restores getHostname at the end of a test.
struct hostnameGuard {
    std::function<std::pair<string, error>()> orig = getHostname;
    explicit hostnameGuard(std::string name) {
        getHostname = [name] { return std::pair<string, error>{string(name), nullptr}; };
    }
    ~hostnameGuard() { getHostname = orig; }
};

const std::array<byte, 4> TestAddr = {0xc0, 0x00, 0x02, 0x01};
const std::array<byte, 16> TestAddr6 = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

dm::Question mustQuestion(std::string_view name, dm::Type qtype, dm::Class cls) {
    dm::Question q;
    q.Name = dm::MustNewName(name);
    q.Type = qtype;
    q.Class = cls;
    return q;
}

dm::Resource resource(const dm::Name& name, ptr<dm::ResourceBody> body) {
    dm::Resource r;
    r.Header.Name = name;
    r.Header.Type = body->realType();
    r.Header.Class = dm::ClassINET;
    r.Body = std::move(body);
    return r;
}

ptr<dm::AResource> aRes(std::array<byte, 4> a) { return make_ptr<dm::AResource>(a); }
ptr<dm::AAAAResource> aaaaRes(std::array<byte, 16> a) { return make_ptr<dm::AAAAResource>(a); }

dm::Message reply(const dm::Message& q, dm::RCode rcode = dm::RCodeSuccess) {
    dm::Message r;
    r.Header.ID = q.Header.ID;
    r.Header.Response = true;
    r.Header.RCode = rcode;
    r.Questions = q.Questions;
    return r;
}

// Go's fakeDNSServer: answers through rh, over a Conn that packs and
// unpacks messages instead of sending them.
using handler = std::function<std::pair<dm::Message, error>(std::string_view n, std::string_view s, const dm::Message& q,
                                                            time::Time t)>;

class fakeDNSConn : public Conn {
public:
    fakeDNSConn(bool tcp, handler rh, std::string n, std::string s)
        : tcp_(tcp), rh_(std::move(rh)), n_(std::move(n)), s_(std::move(s)) {}

    error Close() override { return nullptr; }

    std::pair<isize, error> Read(std::span<byte> b) override {
        if (!buf_.empty()) {
            std::size_t n = Copy(b, std::span<const byte>(buf_));
            buf_ = buf_.Sub(n);
            return {isize(n), nullptr};
        }
        auto [resp, err] = rh_(n_, s_, q_, t_);
        if (err) return {0, err};
        auto [bb, perr] = resp.AppendPack(slice<byte>::make(2, 514));
        if (perr) return {0, errors::New("cannot marshal DNS message: " + std::string(perr.Error().view()))};
        if (tcp_) {
            std::size_t l = bb.size() - 2;
            bb[0] = byte(l >> 8);
            bb[1] = byte(l);
            buf_ = bb;
            return Read(b);
        }
        bb = bb.Sub(2);
        if (b.size() < bb.size()) return {0, errors::New("read would fragment DNS message")};
        Copy(b, std::span<const byte>(bb));
        return {isize(bb.size()), nullptr};
    }

    std::pair<isize, error> Write(std::span<const byte> b) override {
        if (tcp_ && b.size() >= 2) b = b.subspan(2);
        if (q_.Unpack(slice<byte>::of(b))) {
            return {0, errors::New("cannot unmarshal DNS message fake " + n_ + " (" + std::to_string(b.size()) + ")")};
        }
        return {isize(b.size()), nullptr};
    }

    error SetDeadline(time::Time t) override {
        t_ = t;
        return nullptr;
    }
    error SetReadDeadline(time::Time) override { return nullptr; }
    error SetWriteDeadline(time::Time) override { return nullptr; }
    ptr<Addr> LocalAddr() const override { return nullptr; }
    ptr<Addr> RemoteAddr() const override { return nullptr; }

private:
    bool tcp_;
    handler rh_;
    std::string n_, s_;
    dm::Message q_;
    time::Time t_;
    slice<byte> buf_;
};

class fakeDNSPacketConn final : public fakeDNSConn, public PacketConn {
public:
    using fakeDNSConn::fakeDNSConn;
    std::tuple<isize, ptr<Addr>, error> ReadFrom(std::span<byte>) override { return {0, nullptr, errors::New("no")}; }
    std::pair<isize, error> WriteTo(std::span<const byte>, const ptr<Addr>&) override { return {0, errors::New("no")}; }
    error Close() override { return fakeDNSConn::Close(); }
    ptr<Addr> LocalAddr() const override { return nullptr; }
    error SetDeadline(time::Time t) override { return fakeDNSConn::SetDeadline(t); }
    error SetReadDeadline(time::Time t) override { return fakeDNSConn::SetReadDeadline(t); }
    error SetWriteDeadline(time::Time t) override { return fakeDNSConn::SetWriteDeadline(t); }
};

struct fakeDNSServer {
    handler rh;
    bool alwaysTCP = false;

    std::pair<ptr<Conn>, error> DialContext(const ptr<context::Context>&, std::string_view n, std::string_view s) const {
        if (alwaysTCP || n == "tcp" || n == "tcp4" || n == "tcp6") {
            return {make_ptr<fakeDNSConn>(true, rh, std::string(n), std::string(s)), nullptr};
        }
        return {make_ptr<fakeDNSPacketConn>(false, rh, std::string(n), std::string(s)), nullptr};
    }
};

ptr<Resolver> resolverWith(const fakeDNSServer& fake, bool strict = false) {
    auto r = make_ptr<Resolver>();
    r->PreferGo = true;
    r->StrictErrors = strict;
    r->Dial = [fake](const ptr<context::Context>& ctx, std::string_view n, std::string_view s) {
        return fake.DialContext(ctx, n, s);
    };
    return r;
}

const fakeDNSServer fakeDNSServerSuccessful{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
    dm::Message r = reply(q);
    if (q.Questions.size() == 1 && q.Questions[0].Type == dm::TypeA) r.Answers = {resource(q.Questions[0].Name, aRes(TestAddr))};
    return std::pair<dm::Message, error>{r, nullptr};
}};

// Go's resolvConfTest: a resolv.conf in a temporary directory, and the
// system configuration taken from it until teardown.
struct resolvConfTest {
    std::string dir, path;
    resolvConfTest() {
        auto [d, err] = os::MkdirTemp("", "go-resolvconftest");
        EXPECT_FALSE(err);
        dir = std::string(d.view());
        path = dir + "/resolv.conf";
    }
    ~resolvConfTest() {
        resolvConfForceUpdate("");
        os::RemoveAll(dir);
    }
    void write(const std::vector<std::string>& lines) {
        std::string s;
        for (std::size_t i = 0; i < lines.size(); i++) s += (i ? "\n" : "") + lines[i];
        ASSERT_FALSE(os::WriteFile(path, as_bytes(s), 0600));
    }
    void writeAndUpdate(const std::vector<std::string>& lines) {
        write(lines);
        resolvConfForceUpdate(path);
    }
    std::vector<std::string> servers() { return getSystemDNSConfig()->servers; }
};

std::pair<dm::Message, error> ok(dm::Message m) { return {std::move(m), nullptr}; }

}  // namespace

// ---------------------------------------------------------------------------
// dnsclient_test.go

namespace {

void checkDistribution(const std::vector<ptr<SRV>>& data, double margin) {
    int sum = 0;
    for (auto& srv : data) sum += srv->Weight;
    std::map<std::string, int> results;
    constexpr int count = 10000;
    for (int j = 0; j < count; j++) {
        auto d = data;
        shuffleByWeight(d.data(), d.size());
        results[str(d[0]->Target)]++;
    }
    int actual = results[str(data[0]->Target)];
    double expected = double(count) * double(data[0]->Weight) / double(sum);
    double diff = std::abs(double(actual) - expected);
    EXPECT_LE(diff, expected * margin) << "missed target weight: expected " << expected << ", " << actual;
}

std::vector<ptr<SRV>> srvs(std::vector<std::pair<std::string, uint16>> v) {
    std::vector<ptr<SRV>> out;
    for (auto& [t, w] : v) {
        auto s = make_ptr<SRV>();
        s->Target = string(t);
        s->Weight = w;
        out.push_back(s);
    }
    return out;
}

void testUniformity(int size, double margin) {
    std::vector<std::pair<std::string, uint16>> v;
    for (int i = 0; i < size; i++) v.push_back({std::string(1, char('a' + i)), 1});
    checkDistribution(srvs(v), margin);
}

}  // namespace

TEST(GoNetDNS, DNSSRVUniformity) {
    testUniformity(2, 0.05);
    testUniformity(3, 0.10);
    testUniformity(10, 0.20);
    checkDistribution(srvs({{"a", 60}, {"b", 30}, {"c", 10}}), 0.05);
}

TEST(GoNetDNS, Weighting) { checkDistribution(srvs({{"a", 60}, {"b", 30}, {"c", 10}}), 0.05); }

// ---------------------------------------------------------------------------
// dnsconfig_unix_test.go

namespace {

struct dnsConfigWant {
    std::vector<std::string> servers = {}, search = {};
    int ndots = 1;
    time::Duration timeout = 5 * time::Second;
    int attempts = 2;
    bool rotate = false, unknownOpt = false;
    std::vector<std::string> lookup = {};
    bool singleRequest = false, useTCP = false;
};

struct dnsReadConfigTest {
    std::string name;
    dnsConfigWant want;
};

std::vector<dnsReadConfigTest> dnsReadConfigTests() {
    return {
        {"resolv.conf",
         {.servers = {"8.8.8.8:53", "[2001:4860:4860::8888]:53", "[fe80::1%lo0]:53"},
          .search = {"localdomain."},
          .ndots = 5,
          .timeout = 10 * time::Second,
          .attempts = 3,
          .rotate = true,
          .unknownOpt = true}},
        {"domain-resolv.conf", {.servers = {"8.8.8.8:53"}, .search = {"localdomain."}}},
        {"search-resolv.conf", {.servers = {"8.8.8.8:53"}, .search = {"test.", "invalid."}}},
        {"search-single-dot-resolv.conf", {.servers = {"8.8.8.8:53"}, .search = {}}},
        {"empty-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}}},
        {"invalid-ndots-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}, .ndots = 0}},
        {"large-ndots-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}, .ndots = 15}},
        {"negative-ndots-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}, .ndots = 0}},
        {"openbsd-resolv.conf",
         {.servers = {"169.254.169.254:53", "10.240.0.1:53"},
          .search = {"c.symbolic-datum-552.internal."},
          .lookup = {"file", "bind"}}},
        {"single-request-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}, .singleRequest = true}},
        {"single-request-reopen-resolv.conf",
         {.servers = defaultNS, .search = {"domain.local."}, .singleRequest = true}},
        {"linux-use-vc-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}, .useTCP = true}},
        {"freebsd-usevc-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}, .useTCP = true}},
        {"openbsd-tcp-resolv.conf", {.servers = defaultNS, .search = {"domain.local."}, .useTCP = true}},
    };
}

void expectConfig(const dnsConfig& conf, const dnsConfigWant& want, const std::string& name) {
    SCOPED_TRACE(name);
    EXPECT_EQ(conf.servers, want.servers);
    EXPECT_EQ(conf.search, want.search);
    EXPECT_EQ(conf.ndots, want.ndots);
    EXPECT_EQ(conf.timeout.ns, want.timeout.ns);
    EXPECT_EQ(conf.attempts, want.attempts);
    EXPECT_EQ(conf.rotate, want.rotate);
    EXPECT_EQ(conf.unknownOpt, want.unknownOpt);
    EXPECT_EQ(conf.lookup, want.lookup);
    EXPECT_EQ(conf.singleRequest, want.singleRequest);
    EXPECT_EQ(conf.useTCP, want.useTCP);
}

}  // namespace

TEST(GoNetDNS, DNSReadConfig) {
    hostnameGuard g("host.domain.local");
    for (auto& tt : dnsReadConfigTests()) {
        auto want = tt.want;
        if (want.search.empty()) want.search = dnsDefaultSearch();
        auto conf = dnsReadConfig(testdata(tt.name));
        ASSERT_FALSE(conf->err) << conf->err;
        expectConfig(*conf, want, tt.name);
    }
}

TEST(GoNetDNS, DNSReadMissingFile) {
    hostnameGuard g("host.domain.local");
    auto conf = dnsReadConfig("a-nonexistent-file");
    EXPECT_TRUE(os::IsNotExist(conf->err)) << conf->err;
    expectConfig(*conf, {.servers = defaultNS, .search = {"domain.local."}}, "missing");
}

TEST(GoNetDNS, DNSDefaultSearch) {
    auto orig = getHostname;
    struct {
        std::string name;
        error err;
        std::vector<std::string> want;
    } tests[] = {
        {"host.long.domain.local", nullptr, {"long.domain.local."}},
        {"host.local", nullptr, {"local."}},
        {"host", nullptr, {}},
        {"host.domain.local", errors::New("errored"), {}},
        // Ensures we don't return {""}, which causes duplicate lookups.
        {"foo.", nullptr, {}},
    };
    for (auto& tt : tests) {
        getHostname = [&] { return std::pair<string, error>{string(tt.name), tt.err}; };
        EXPECT_EQ(dnsDefaultSearch(), tt.want) << tt.name;
    }
    getHostname = orig;
}

TEST(GoNetDNS, DNSNameLength) {
    hostnameGuard g("host.domain.local");
    std::string char63(63, 'a');
    std::string longDomain;
    for (int i = 0; i < 5; i++) longDomain += char63 + ".";
    longDomain += "example";
    for (auto& tt : dnsReadConfigTests()) {
        auto conf = dnsReadConfig(testdata(tt.name));
        ASSERT_FALSE(conf->err);
        auto suffixList = tt.want.search;
        if (suffixList.empty()) suffixList = dnsDefaultSearch();
        std::size_t shortestSuffix = 0;
        for (auto& suffix : suffixList) {
            if (shortestSuffix == 0 || suffix.size() < shortestSuffix) shortestSuffix = suffix.size();
        }
        // A name that is maximally long when prefixing the shortest suffix
        // (accounting for the intervening dot).
        std::string longName = longDomain.substr(longDomain.size() - 254 + 1 + shortestSuffix);
        if (longName[0] == '.' || longName[1] == '.') longName = "aa." + longName.substr(3);
        for (auto& fqdn : conf->nameList(longName)) EXPECT_LE(fqdn.size(), 254u) << tt.name;
        // A name that's too long for suffixing.
        std::string unsuffixable = "a." + longName.substr(1);
        EXPECT_EQ(conf->nameList(unsuffixable).size(), 1u) << tt.name;
        // A name that's too long for DNS.
        EXPECT_TRUE(conf->nameList("a." + longDomain).empty()) << tt.name;
    }
}

// ---------------------------------------------------------------------------
// hosts_test.go

namespace {

struct staticHostEntry {
    std::string in;
    std::vector<std::string> out;
};

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void testStaticHost(const std::string& hostsPath, const staticHostEntry& ent) {
    for (auto& in : {ent.in, absDomainName(ent.in), lowerASCII(ent.in), upper(ent.in)}) {
        auto [addrs, canonical] = lookupStaticHost(in);
        EXPECT_EQ(addrs, ent.out) << hostsPath << ", lookupStaticHost(" << in << ")";
    }
}

void testStaticAddr(const std::string& hostsPath, staticHostEntry ent) {
    auto hosts = lookupStaticAddr(ent.in);
    for (auto& o : ent.out) o = absDomainName(o);
    EXPECT_EQ(hosts, ent.out) << hostsPath << ", lookupStaticAddr(" << ent.in << ")";
}

struct lookupStaticHostAliasesEntry {
    std::string lookup, res;
};

const std::vector<lookupStaticHostAliasesEntry> lookupStaticHostAliasesTest = {
    // 127.0.0.1
    {"test", "test"},
    // 127.0.0.2
    {"test2.example.com", "test2.example.com"},
    {"2.test", "test2.example.com"},
    // 127.0.0.3
    {"test3.example.com", "3.test"},
    {"3.test", "3.test"},
    // 127.0.0.4
    {"example.com", "example.com"},
    // 127.0.0.5
    {"test5.example.com", "test4.example.com"},
    {"5.test", "test4.example.com"},
    {"4.test", "test4.example.com"},
    {"test4.example.com", "test4.example.com"},
};

}  // namespace

TEST(GoNetDNS, LookupStaticHost) {
    hostsFileGuard g;
    struct {
        std::string name;
        std::vector<staticHostEntry> ents;
    } tests[] = {
        {"hosts",
         {{"odin", {"127.0.0.2", "127.0.0.3", "::2"}},
          {"thor", {"127.1.1.1"}},
          {"ullr", {"127.1.1.2"}},
          {"ullrhost", {"127.1.1.2"}},
          {"localhost", {"fe80::1%lo0"}}}},
        {"singleline-hosts", {{"odin", {"127.0.0.2"}}}},  // go.dev/issue/6646
        {"ipv4-hosts",
         {{"localhost", {"127.0.0.1", "127.0.0.2", "127.0.0.3"}}, {"localhost.localdomain", {"127.0.0.3"}}}},
        {"ipv6-hosts",  // go.dev/issue/8996
         {{"localhost", {"::1", "fe80::1", "fe80::2%lo0", "fe80::3%lo0"}}, {"localhost.localdomain", {"fe80::3%lo0"}}}},
        {"case-hosts",  // go.dev/issue/12806
         {{"PreserveMe", {"127.0.0.1", "::1"}}, {"PreserveMe.local", {"127.0.0.1", "::1"}}}},
    };
    for (auto& tt : tests) {
        hostsFilePath = testdata(tt.name);
        for (auto& ent : tt.ents) testStaticHost(tt.name, ent);
    }
}

TEST(GoNetDNS, LookupStaticAddr) {
    hostsFileGuard g;
    struct {
        std::string name;
        std::vector<staticHostEntry> ents;
    } tests[] = {
        {"hosts",
         {{"255.255.255.255", {"broadcasthost"}},
          {"127.0.0.2", {"odin"}},
          {"127.0.0.3", {"odin"}},
          {"::2", {"odin"}},
          {"127.1.1.1", {"thor"}},
          {"127.1.1.2", {"ullr", "ullrhost"}},
          {"fe80::1%lo0", {"localhost"}}}},
        {"singleline-hosts", {{"127.0.0.2", {"odin"}}}},
        {"ipv4-hosts",
         {{"127.0.0.1", {"localhost"}},
          {"127.0.0.2", {"localhost"}},
          {"127.0.0.3", {"localhost", "localhost.localdomain"}}}},
        {"ipv6-hosts",
         {{"::1", {"localhost"}},
          {"fe80::1", {"localhost"}},
          {"fe80::2%lo0", {"localhost"}},
          {"fe80::3%lo0", {"localhost", "localhost.localdomain"}}}},
        {"case-hosts",
         {{"127.0.0.1", {"PreserveMe", "PreserveMe.local"}}, {"::1", {"PreserveMe", "PreserveMe.local"}}}},
    };
    for (auto& tt : tests) {
        hostsFilePath = testdata(tt.name);
        for (auto& ent : tt.ents) testStaticAddr(tt.name, ent);
    }
}

TEST(GoNetDNS, HostCacheModification) {
    // Programs can't modify the internals of the host cache
    // (go.dev/issue/14212).
    hostsFileGuard g;
    hostsFilePath = testdata("ipv4-hosts");
    staticHostEntry ent{"localhost", {"127.0.0.1", "127.0.0.2", "127.0.0.3"}};
    testStaticHost(hostsFilePath, ent);
    auto [addrs, c] = lookupStaticHost(ent.in);
    for (auto& a : addrs) a += "junk";
    testStaticHost(hostsFilePath, ent);

    hostsFilePath = testdata("ipv6-hosts");
    ent = {"::1", {"localhost"}};
    testStaticAddr(hostsFilePath, ent);
    auto hosts = lookupStaticAddr(ent.in);
    for (auto& h : hosts) h += "junk";
    testStaticAddr(hostsFilePath, ent);
}

TEST(GoNetDNS, LookupStaticHostAliases) {
    hostsFileGuard g;
    hostsFilePath = testdata("aliases");
    for (auto& ent : lookupStaticHostAliasesTest) {
        for (auto& in : {ent.lookup, absDomainName(ent.lookup), lowerASCII(ent.lookup), upper(ent.lookup)}) {
            auto [addrs, res] = lookupStaticHost(in);
            EXPECT_EQ(res, absDomainName(ent.res)) << "lookupStaticHost(" << in << ")";
        }
    }
}

// ---------------------------------------------------------------------------
// nss_test.go

TEST(GoNetDNS, ParseNSSConf) {
    const std::string ubuntuTrustyAvahi = R"(# /etc/nsswitch.conf
#
# Example configuration of GNU Name Service Switch functionality.
# If you have the libc-doc-reference' and nfo' packages installed, try:
# nfo libc "Name Service Switch"' for information about this file.

passwd:         compat
group:          compat
shadow:         compat

hosts:          files mdns4_minimal [NOTFOUND=return] dns mdns4
networks:       files

protocols:      db files
services:       db files
ethers:         db files
rpc:            db files

netgroup:       nis
)";
    using sources = std::map<std::string, std::vector<nssSource>>;
    struct {
        std::string name, in;
        sources want;
    } tests[] = {
        {"no_newline", "foo: a b", {{"foo", {{"a", {}}, {"b", {}}}}}},
        {"newline", "foo: a b\n", {{"foo", {{"a", {}}, {"b", {}}}}}},
        {"whitespace", "   foo:a    b    \n", {{"foo", {{"a", {}}, {"b", {}}}}}},
        {"comment1", "   foo:a    b#c\n", {{"foo", {{"a", {}}, {"b", {}}}}}},
        {"comment2", "   foo:a    b #c \n", {{"foo", {{"a", {}}, {"b", {}}}}}},
        {"crit",
         "   foo:a    b [!a=b    X=Y ] c#d \n",
         {{"foo", {{"a", {}}, {"b", {{true, "a", "b"}, {false, "x", "y"}}}, {"c", {}}}}}},
        // Ubuntu Trusty w/ avahi-daemon, libavahi-* etc installed.
        {"ubuntu_trusty_avahi",
         ubuntuTrustyAvahi,
         {{"passwd", {{"compat", {}}}},
          {"group", {{"compat", {}}}},
          {"shadow", {{"compat", {}}}},
          {"hosts", {{"files", {}}, {"mdns4_minimal", {{false, "notfound", "return"}}}, {"dns", {}}, {"mdns4", {}}}},
          {"networks", {{"files", {}}}},
          {"protocols", {{"db", {}}, {"files", {}}}},
          {"services", {{"db", {}}, {"files", {}}}},
          {"ethers", {{"db", {}}, {"files", {}}}},
          {"rpc", {{"db", {}}, {"files", {}}}},
          {"netgroup", {{"nis", {}}}}}},
    };
    for (auto& tt : tests) {
        auto got = parseNSSConf(tt.in);
        EXPECT_FALSE(got->err) << tt.name;
        EXPECT_TRUE(got->sources == tt.want) << tt.name;
    }
}

// ---------------------------------------------------------------------------
// addrselect_test.go

namespace {
IPAddr ipa(std::string_view s) { return IPAddr(ParseIP(s)); }
netip::Addr na(std::string_view s) { return netip::MustParseAddr(s); }
std::string render(const std::vector<IPAddr>& v) {
    std::string s;
    for (auto& a : v) s += str(a.String()) + " ";
    return s;
}
}  // namespace

TEST(GoNetDNS, SortByRFC6724) {
    struct {
        std::vector<IPAddr> in;
        std::vector<netip::Addr> srcs;
        std::vector<IPAddr> want;
        bool reverse;
    } tests[] = {
        // Examples from RFC 6724 section 10.2: prefer matching scope.
        {{ipa("2001:db8:1::1"), ipa("198.51.100.121")},
         {na("2001:db8:1::2"), na("169.254.13.78")},
         {ipa("2001:db8:1::1"), ipa("198.51.100.121")},
         true},
        // Prefer matching scope.
        {{ipa("2001:db8:1::1"), ipa("198.51.100.121")},
         {na("fe80::1"), na("198.51.100.117")},
         {ipa("198.51.100.121"), ipa("2001:db8:1::1")},
         true},
        // Prefer higher precedence.
        {{ipa("2001:db8:1::1"), ipa("10.1.2.3")},
         {na("2001:db8:1::2"), na("10.1.2.4")},
         {ipa("2001:db8:1::1"), ipa("10.1.2.3")},
         true},
        // Prefer smaller scope.
        {{ipa("2001:db8:1::1"), ipa("fe80::1")},
         {na("2001:db8:1::2"), na("fe80::2")},
         {ipa("fe80::1"), ipa("2001:db8:1::1")},
         true},
        // Issue 13283: a 10/8 source doesn't mean preferring 23/8.
        {{ipa("54.83.193.112"), ipa("184.72.238.214"), ipa("23.23.172.185"), ipa("75.101.148.21"),
          ipa("23.23.134.56"), ipa("23.21.50.150")},
         {na("10.2.3.4"), na("10.2.3.4"), na("10.2.3.4"), na("10.2.3.4"), na("10.2.3.4"), na("10.2.3.4")},
         {ipa("54.83.193.112"), ipa("184.72.238.214"), ipa("23.23.172.185"), ipa("75.101.148.21"),
          ipa("23.23.134.56"), ipa("23.21.50.150")},
         false},
    };
    int i = 0;
    for (auto& tt : tests) {
        auto in = tt.in;
        auto srcs = tt.srcs;
        sortByRFC6724withSrcs(in, srcs);
        EXPECT_EQ(render(in), render(tt.want)) << "test " << i;
        if (tt.reverse) {
            in = tt.in;
            srcs = tt.srcs;
            std::reverse(in.begin(), in.end());
            std::reverse(srcs.begin(), srcs.end());
            sortByRFC6724withSrcs(in, srcs);
            EXPECT_EQ(render(in), render(tt.want)) << "test " << i << ", starting backwards";
        }
        i++;
    }
}

TEST(GoNetDNS, RFC6724PolicyTableOrder) {
    auto& t = rfc6724policyTable();
    for (std::size_t i = 0; i + 1 < t.size(); i++) EXPECT_GE(t[i].Prefix.Bits(), t[i + 1].Prefix.Bits()) << i;
}

TEST(GoNetDNS, RFC6724PolicyTableContent) {
    struct {
        std::string prefix;
        uint8 precedence, label;
    } want[] = {
        {"::1/128", 50, 0},  {"::ffff:0:0/96", 35, 4}, {"::/96", 1, 3},     {"2001::/32", 5, 5}, {"2002::/16", 30, 2},
        {"3ffe::/16", 1, 12}, {"fec0::/10", 1, 11},    {"fc00::/7", 3, 13}, {"::/0", 40, 1},
    };
    auto& t = rfc6724policyTable();
    ASSERT_EQ(t.size(), std::size(want));
    for (std::size_t i = 0; i < t.size(); i++) {
        EXPECT_EQ(t[i].Prefix, netip::MustParsePrefix(want[i].prefix)) << i;
        EXPECT_EQ(t[i].Precedence, want[i].precedence) << i;
        EXPECT_EQ(t[i].Label, want[i].label) << i;
    }
}

TEST(GoNetDNS, RFC6724PolicyTableClassify) {
    struct {
        netip::Addr ip;
        std::string prefix;
        uint8 precedence, label;
    } tests[] = {
        {na("127.0.0.1"), "::ffff:0:0/96", 35, 4},
        {na("2601:645:8002:a500:986f:1db8:c836:bd65"), "::/0", 40, 1},
        {na("::1"), "::1/128", 50, 0},
        {na("2002::ab12"), "2002::/16", 30, 2},
    };
    for (auto& tt : tests) {
        auto got = classifyPolicy(tt.ip);
        EXPECT_EQ(got.Prefix, netip::MustParsePrefix(tt.prefix));
        EXPECT_EQ(got.Precedence, tt.precedence);
        EXPECT_EQ(got.Label, tt.label);
    }
}

TEST(GoNetDNS, RFC6724ClassifyScope) {
    struct {
        netip::Addr ip;
        scope want;
    } tests[] = {
        {na("127.0.0.1"), scopeLinkLocal},
        {na("::1"), scopeLinkLocal},
        {na("169.254.1.2"), scopeLinkLocal},
        {na("fec0::1"), scopeSiteLocal},
        {na("8.8.8.8"), scopeGlobal},
        {na("ff02::"), scopeLinkLocal},
        {na("ff05::"), scopeSiteLocal},
        {na("ff04::"), scopeAdminLocal},
        {na("ff0e::"), scopeGlobal},
        {netip::AddrFrom16({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xe0, 0, 0, 0}), scopeGlobal},
        {netip::AddrFrom16({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xe0, 2, 2, 2}), scopeGlobal},
        {netip::AddrFrom4({0xe0, 0, 0, 0}), scopeGlobal},
        {netip::AddrFrom4({0xe0, 2, 2, 2}), scopeGlobal},
    };
    int i = 0;
    for (auto& tt : tests) EXPECT_EQ(classifyScope(tt.ip), tt.want) << i++;
}

TEST(GoNetDNS, RFC6724CommonPrefixLength) {
    struct {
        netip::Addr a;
        IP b;
        int want;
    } tests[] = {
        {na("fe80::1"), ParseIP("fe80::2"), 64},
        {na("fe81::1"), ParseIP("fe80::2"), 15},
        {na("127.0.0.1"), ParseIP("fe80::1"), 0},  // diff size
        {netip::AddrFrom4({1, 2, 3, 4}), IP{1, 2, 3, 4}, 32},
        {netip::AddrFrom4({1, 2, 255, 255}), IP{1, 2, 0, 0}, 16},
        {netip::AddrFrom4({1, 2, 127, 255}), IP{1, 2, 0, 0}, 17},
        {netip::AddrFrom4({1, 2, 63, 255}), IP{1, 2, 0, 0}, 18},
        {netip::AddrFrom4({1, 2, 31, 255}), IP{1, 2, 0, 0}, 19},
        {netip::AddrFrom4({1, 2, 15, 255}), IP{1, 2, 0, 0}, 20},
        {netip::AddrFrom4({1, 2, 7, 255}), IP{1, 2, 0, 0}, 21},
        {netip::AddrFrom4({1, 2, 3, 255}), IP{1, 2, 0, 0}, 22},
        {netip::AddrFrom4({1, 2, 1, 255}), IP{1, 2, 0, 0}, 23},
        {netip::AddrFrom4({1, 2, 0, 255}), IP{1, 2, 0, 0}, 24},
    };
    int i = 0;
    for (auto& tt : tests) EXPECT_EQ(commonPrefixLen(tt.a, tt.b), tt.want) << i++;
}

// ---------------------------------------------------------------------------
// dnsclient_unix_test.go

TEST(GoNetDNS, DNSTransportFallback) {
    // A truncated UDP answer is asked again over TCP.
    fakeDNSServer fake{[](std::string_view n, std::string_view, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        if (n == "udp") r.Header.Truncated = true;
        return ok(r);
    }};
    auto r = resolverWith(fake);
    for (std::string server : {"8.8.8.8:53", "8.8.4.4:53"}) {
        auto [p, h, err] = exchange(r.get(), context::Background(), server,
                                    mustQuestion("com.", dm::TypeALL, dm::ClassINET), time::Second, false, false);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(h.RCode, dm::RCodeSuccess);
    }
}

TEST(GoNetDNS, DNSTransportNoFallbackOnTCP) {
    fakeDNSServer fake{[](std::string_view n, std::string_view, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        r.Header.Truncated = true;
        if (n == "tcp") r.Answers = {resource(q.Questions[0].Name, aRes(TestAddr))};
        return ok(r);
    }};
    auto r = resolverWith(fake);
    for (std::string server : {"8.8.8.8:53", "8.8.4.4:53"}) {
        auto [p, h, err] = exchange(r.get(), context::Background(), server,
                                    mustQuestion("com.", dm::TypeALL, dm::ClassINET), time::Second, false, false);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(h.RCode, dm::RCodeSuccess);
        auto [a, aerr] = p.AllAnswers();
        ASSERT_FALSE(aerr) << aerr;
        EXPECT_EQ(a.size(), 1u);
    }
}

TEST(GoNetDNS, SpecialDomainName) {
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        return ok(reply(q, q.Questions[0].Name.String() == "example.com." ? dm::RCodeSuccess : dm::RCodeNameError));
    }};
    auto r = resolverWith(fake);
    struct {
        dm::Question question;
        dm::RCode rcode;
    } tests[] = {
        {mustQuestion("1.0.168.192.in-addr.arpa.", dm::TypePTR, dm::ClassINET), dm::RCodeNameError},
        {mustQuestion("test.", dm::TypeALL, dm::ClassINET), dm::RCodeNameError},
        {mustQuestion("example.com.", dm::TypeALL, dm::ClassINET), dm::RCodeSuccess},
        {mustQuestion("localhost.", dm::TypeALL, dm::ClassINET), dm::RCodeNameError},
        {mustQuestion("invalid.", dm::TypeALL, dm::ClassINET), dm::RCodeNameError},
    };
    for (auto& tt : tests) {
        auto [p, h, err] = exchange(r.get(), context::Background(), "8.8.8.8:53", tt.question, 3 * time::Second,
                                    false, false);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(h.RCode, tt.rcode) << tt.question.Name.String();
    }
}

TEST(GoNetDNS, AvoidDNSName) {
    struct {
        std::string name;
        bool avoid;
    } tests[] = {
        {"foo.com", false},   {"foo.com.", false},   {"foo.onion.", true},  {"foo.onion", true},
        {"foo.ONION", true},  {"foo.ONION.", true},  {"foo.local.", false}, {"foo.local", false},
        {"foo.LOCAL", false}, {"foo.LOCAL.", false}, {"", true},            {"local", false},
        {"onion", false},     {"local.", false},     {"onion.", false},
    };
    for (auto& tt : tests) EXPECT_EQ(avoidDNS(tt.name), tt.avoid) << tt.name;
}

TEST(GoNetDNS, NameListAvoidDNS) {
    dnsConfig c;
    c.ndots = 0;  // Go's dnsConfig literal has the zero value
    c.search = {"go.dev.", "onion."};
    EXPECT_EQ(c.nameList("www"), (std::vector<std::string>{"www.", "www.go.dev."}));
    EXPECT_EQ(c.nameList("www.onion"), (std::vector<std::string>{"www.onion.go.dev."}));
}

TEST(GoNetDNS, LookupTorOnion) {
    auto r = resolverWith(fakeDNSServerSuccessful);
    auto [addrs, err] = r->LookupIPAddr(context::Background(), "foo.onion.");
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(addrs.empty());
}

TEST(GoNetDNS, UpdateResolvConf) {
    auto r = resolverWith(fakeDNSServerSuccessful);
    resolvConfTest conf;
    struct {
        std::string name;
        std::vector<std::string> lines, servers;
    } tests[] = {
        {"golang.org", {"nameserver 8.8.8.8"}, {"8.8.8.8:53"}},
        // An empty resolv.conf uses defaultNS.
        {"", {}, defaultNS},
        {"www.example.com", {"nameserver 8.8.4.4"}, {"8.8.4.4:53"}},
    };
    for (auto& tt : tests) {
        conf.writeAndUpdate(tt.lines);
        if (!tt.name.empty()) {
            sync::WaitGroup wg;
            constexpr int N = 10;
            for (int j = 0; j < N; j++) {
                wg.Go([&r, name = tt.name] {
                    auto [ips, err] = r->LookupIPAddr(context::Background(), name);
                    EXPECT_FALSE(err) << err;
                    EXPECT_FALSE(ips.empty()) << "no records for " << name;
                });
            }
            wg.Wait();
        }
        EXPECT_EQ(conf.servers(), tt.servers);
    }
}

TEST(GoNetDNS, GoLookupIPWithResolverConfig) {
    fakeDNSServer fake{[](std::string_view, std::string_view s, const dm::Message& q, time::Time) {
        if (s != "[2001:4860:4860::8888]:53" && s != "8.8.8.8:53") {
            time::Sleep(10ms);
            return std::pair<dm::Message, error>{dm::Message{}, os::ErrDeadlineExceeded};
        }
        dm::Message r = reply(q);
        for (auto& question : q.Questions) {
            std::string name = question.Name.String();
            if (question.Type == dm::TypeA && name == "ipv4.google.com.") {
                r.Answers.push_back(resource(q.Questions[0].Name, aRes(TestAddr)));
            }
            if (question.Type == dm::TypeAAAA && name == "ipv6.google.com.") {
                r.Answers.push_back(resource(q.Questions[0].Name, aaaaRes(TestAddr6)));
            }
        }
        return ok(r);
    }};
    auto r = resolverWith(fake);
    resolvConfTest conf;
    struct {
        std::string name;
        std::vector<std::string> lines;
        bool wantErr;
        std::string errServer;
        bool errTimeout;
        bool a, aaaa;
    } tests[] = {
        // No records, transport timeout.
        {"jgahvsekduiv9bw4b3qhn4ykdfgj0493iohkrjfhdvhjiu4j",
         {"options timeout:1 attempts:1", "nameserver 255.255.255.255"},
         true,
         "255.255.255.255:53",
         true,
         false,
         false},
        // No records, non-existent domain.
        {"jgahvsekduiv9bw4b3qhn4ykdfgj0493iohkrjfhdvhjiu4j",
         {"options timeout:3 attempts:1", "nameserver 8.8.8.8"},
         true,
         "8.8.8.8:53",
         false,
         false,
         false},
        // A few A records, no AAAA records.
        {"ipv4.google.com.", {"nameserver 8.8.8.8", "nameserver 2001:4860:4860::8888"}, false, "", false, true, false},
        {"ipv4.google.com",
         {"domain golang.org", "nameserver 2001:4860:4860::8888", "nameserver 8.8.8.8"},
         false, "", false, true, false},
        {"ipv4.google.com",
         {"search x.golang.org y.golang.org", "nameserver 2001:4860:4860::8888", "nameserver 8.8.8.8"},
         false, "", false, true, false},
        // No A records, a few AAAA records.
        {"ipv6.google.com.", {"nameserver 2001:4860:4860::8888", "nameserver 8.8.8.8"}, false, "", false, false, true},
        {"ipv6.google.com",
         {"domain golang.org", "nameserver 8.8.8.8", "nameserver 2001:4860:4860::8888"},
         false, "", false, false, true},
        {"ipv6.google.com",
         {"search x.golang.org y.golang.org", "nameserver 8.8.8.8", "nameserver 2001:4860:4860::8888"},
         false, "", false, false, true},
    };
    for (auto& tt : tests) {
        SCOPED_TRACE(tt.name);
        conf.writeAndUpdate(tt.lines);
        auto [addrs, err] = r->LookupIPAddr(context::Background(), tt.name);
        if (err) {
            auto* de = errors::As<DNSError>(err);
            ASSERT_TRUE(de) << err;
            EXPECT_TRUE(tt.wantErr) << err;
            EXPECT_EQ(str(de->Name), tt.name);
            EXPECT_EQ(str(de->Server), tt.errServer);
            EXPECT_EQ(de->IsTimeout, tt.errTimeout);
            continue;
        }
        EXPECT_FALSE(addrs.empty()) << "no records";
        for (auto& addr : addrs) {
            if (!tt.a) { EXPECT_TRUE(addr.IP.To4().empty()) << "got " << str(addr.String()) << "; must not be IPv4"; }
            if (!tt.aaaa) { EXPECT_FALSE(addr.IP.To4().empty()) << "got " << str(addr.String()) << "; must not be IPv6"; }
        }
    }
}

TEST(GoNetDNS, GoLookupIPOrderFallbackToFile) {
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) { return ok(reply(q)); }};
    auto r = resolverWith(fake);
    // A config that simulates no DNS servers being available.
    resolvConfTest conf;
    conf.writeAndUpdate({});
    hostsFileGuard g;
    hostsFilePath = testdata("hosts");
    for (auto order : {hostLookupFilesDNS, hostLookupDNSFiles}) {
        SCOPED_TRACE(hostLookupOrderString(order));
        // An error for a host that doesn't exist.
        auto [a1, c1, err1] = goLookupIPCNAMEOrder(r.get(), context::Background(), "ip", "notarealhost", order, nullptr);
        EXPECT_TRUE(err1) << "expected error while looking up name not in hosts file";
        // An address for a name in the hosts file.
        auto [addrs, c2, err2] = goLookupIPCNAMEOrder(r.get(), context::Background(), "ip", "thor", order, nullptr);
        ASSERT_FALSE(err2) << err2;
        ASSERT_EQ(addrs.size(), 1u);
        EXPECT_EQ(str(addrs[0].String()), "127.1.1.1");
    }
}

TEST(GoNetDNS, ErrorForOriginalNameWhenSearching) {
    // go.dev/issue/12712: the error of the original name, not of a name
    // made with the search list.
    const std::string fqdn = "doesnotexist.domain";
    resolvConfTest conf;
    conf.writeAndUpdate({"search servfail"});
    fakeDNSServer fake{[fqdn](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        return ok(reply(q, q.Questions[0].Name.String() == fqdn + ".servfail." ? dm::RCodeServerFailure
                                                                                : dm::RCodeNameError));
    }};
    struct {
        bool strictErrors;
        std::string err;
        bool temporary;
    } cases[] = {{true, "server misbehaving", true}, {false, "no such host", false}};
    for (auto& tt : cases) {
        auto r = resolverWith(fake, tt.strictErrors);
        auto [addrs, err] = r->LookupIPAddr(context::Background(), fqdn);
        ASSERT_TRUE(err) << "expected an error";
        auto* de = errors::As<DNSError>(err);
        ASSERT_TRUE(de) << err;
        EXPECT_EQ(str(de->Name), fqdn);
        EXPECT_EQ(str(de->Err), tt.err);
        EXPECT_EQ(de->IsTemporary, tt.temporary);
    }
}

TEST(GoNetDNS, IgnoreLameReferrals) {
    // go.dev/issue/15434: a lame referral goes on to the next server.
    resolvConfTest conf;
    conf.writeAndUpdate({"nameserver 192.0.2.1", "nameserver 192.0.2.2"});
    fakeDNSServer fake{[](std::string_view, std::string_view s, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        if (s == "192.0.2.2:53") {
            r.Header.RecursionAvailable = true;
            if (q.Questions[0].Type == dm::TypeA) r.Answers = {resource(q.Questions[0].Name, aRes(TestAddr))};
        } else if (s == "192.0.2.1:53") {
            if (q.Questions[0].Type == dm::TypeA && q.Questions[0].Name.String().starts_with("empty.com.")) {
                dm::Resource opt;
                opt.Header.SetEDNS0(1232, dm::RCodeSuccess, false);
                opt.Body = make_ptr<dm::OPTResource>();
                r.Additionals = {opt};
            }
        }
        return ok(r);
    }};
    auto r = resolverWith(fake);
    auto [addrs, err] = r->LookupIP(context::Background(), "ip4", "www.golang.org");
    ASSERT_FALSE(err) << err;
    ASSERT_EQ(addrs.size(), 1u);
    EXPECT_EQ(str(addrs[0].String()), "192.0.2.1");
    auto [a2, err2] = r->LookupIP(context::Background(), "ip4", "empty.com");
    auto* de = errors::As<DNSError>(err2);
    ASSERT_TRUE(de) << err2;
    EXPECT_EQ(str(de->Err), "no such host");
}

TEST(GoNetDNS, IgnoreDNSForgeries) {
    // go.dev/issue/13281: the UDP round trip ignores invalid answers.
    auto [c, s] = Pipe();
    go([s = s] {
        auto b = slice<byte>::make(1232);
        auto [n, err] = s->Read(b);
        if (err) {
            ADD_FAILURE() << err;
            return;
        }
        dm::Message msg;
        if (msg.Unpack(b.Sub(0, std::size_t(n)))) {
            ADD_FAILURE() << "invalid DNS query";
            return;
        }
        io::WriteString(*s, "garbage DNS response packet");
        msg.Header.Response = true;
        msg.Header.ID++;  // make invalid ID
        auto [b2, perr] = msg.Pack();
        s->Write(b2);
        msg.Header.ID--;  // restore original ID
        msg.Answers = {resource(dm::MustNewName("www.example.com."), aRes(TestAddr))};
        auto [b3, perr2] = msg.Pack();
        s->Write(b3);
    });
    dm::Message msg;
    msg.Header.ID = 42;
    msg.Questions = {mustQuestion("www.example.com.", dm::TypeA, dm::ClassINET)};
    auto [b, err] = msg.Pack();
    ASSERT_FALSE(err);
    // exchange's packet round trip, through a Conn that is a PacketConn.
    struct pipePacketConn final : Conn, PacketConn {
        ptr<Conn> c;
        explicit pipePacketConn(ptr<Conn> c_) : c(std::move(c_)) {}
        std::pair<isize, error> Read(std::span<byte> p) override { return c->Read(p); }
        std::pair<isize, error> Write(std::span<const byte> p) override { return c->Write(p); }
        error Close() override { return c->Close(); }
        ptr<Addr> LocalAddr() const override { return nullptr; }
        ptr<Addr> RemoteAddr() const override { return nullptr; }
        error SetDeadline(time::Time) override { return nullptr; }
        error SetReadDeadline(time::Time) override { return nullptr; }
        error SetWriteDeadline(time::Time) override { return nullptr; }
        std::tuple<isize, ptr<Addr>, error> ReadFrom(std::span<byte>) override { return {0, nullptr, nullptr}; }
        std::pair<isize, error> WriteTo(std::span<const byte>, const ptr<Addr>&) override { return {0, nullptr}; }
    };
    auto pc = make_ptr<pipePacketConn>(c);
    auto r = make_ptr<Resolver>();
    r->Dial = [pc](const ptr<context::Context>&, std::string_view, std::string_view) {
        return std::pair<ptr<Conn>, error>{pc, nullptr};
    };
    // exchange makes its own query with a random ID; answer that one.
    auto [p, h, xerr] = exchange(r.get(), context::Background(), "192.0.2.1:53", msg.Questions[0], time::Second, false,
                                 false);
    ASSERT_FALSE(xerr) << xerr;
    auto [as, aerr] = p.AllAnswers();
    ASSERT_FALSE(aerr) << aerr;
    ASSERT_EQ(as.size(), 1u);
    EXPECT_EQ(static_cast<dm::AResource*>(as[0].Body.get())->A, TestAddr);
}

TEST(GoNetDNS, RetryTimeout) {
    // go.dev/issue/16865: a server that times out is followed by the next.
    resolvConfTest conf;
    conf.writeAndUpdate({"nameserver 192.0.2.1", "nameserver 192.0.2.2"});
    time::Time deadline0;
    auto mockTXT = [](const dm::Message& q) {
        dm::Message r = reply(q);
        r.Header.RecursionAvailable = true;
        r.Answers = {resource(q.Questions[0].Name, make_ptr<dm::TXTResource>(std::vector<std::string>{"ok"}))};
        return r;
    };
    fakeDNSServer fake{[&](std::string_view, std::string_view s, const dm::Message& q, time::Time deadline) {
        EXPECT_FALSE(deadline.IsZero()) << "zero deadline";
        if (s == "192.0.2.1:53") {
            deadline0 = deadline;
            time::Sleep(10ms);
            return std::pair<dm::Message, error>{dm::Message{}, os::ErrDeadlineExceeded};
        }
        EXPECT_FALSE(deadline.Equal(deadline0)) << "deadline didn't change";
        return ok(mockTXT(q));
    }};
    auto r = resolverWith(fake);
    auto [txt, err] = r->LookupTXT(context::Background(), "www.golang.org");
    ASSERT_FALSE(err) << err;
    EXPECT_FALSE(deadline0.IsZero());
}

namespace {

dm::Message mockTXTResponse(const dm::Message& q) {
    dm::Message r = reply(q);
    r.Header.RecursionAvailable = true;
    r.Answers = {resource(q.Questions[0].Name, make_ptr<dm::TXTResource>(std::vector<std::string>{"ok"}))};
    return r;
}

void testRotate(bool rotate, std::vector<std::string> nameservers, std::vector<std::string> wantServers) {
    resolvConfTest conf;
    std::vector<std::string> lines;
    for (auto& ns : nameservers) lines.push_back("nameserver " + ns);
    if (rotate) lines.push_back("options rotate");
    conf.writeAndUpdate(lines);
    std::vector<std::string> usedServers;
    fakeDNSServer fake{[&](std::string_view, std::string_view s, const dm::Message& q, time::Time) {
        usedServers.emplace_back(s);
        return ok(mockTXTResponse(q));
    }};
    auto r = resolverWith(fake);
    // len(nameservers) + 1 to allow rotation to get back to start.
    for (std::size_t i = 0; i < nameservers.size() + 1; i++) {
        auto [txt, err] = r->LookupTXT(context::Background(), "www.golang.org");
        ASSERT_FALSE(err) << err;
    }
    EXPECT_EQ(usedServers, wantServers) << "rotate=" << rotate;
}

}  // namespace

TEST(GoNetDNS, Rotate) {
    // Without rotation, always uses the first server.
    testRotate(false, {"192.0.2.1", "192.0.2.2"}, {"192.0.2.1:53", "192.0.2.1:53", "192.0.2.1:53"});
    // With rotation, rotates through back to first.
    testRotate(true, {"192.0.2.1", "192.0.2.2"}, {"192.0.2.1:53", "192.0.2.2:53", "192.0.2.1:53"});
}

TEST(GoNetDNS, StrictErrorsLookupIP) {
    // go.dev/issue/17448: with StrictErrors, temporary errors make
    // LookupIP fail rather than return a partial result.
    resolvConfTest conf;
    conf.writeAndUpdate({"nameserver 192.0.2.53", "search x.golang.org y.golang.org"});
    const std::string name = "test-issue19592", server = "192.0.2.53:53";
    const std::string searchX = "test-issue19592.x.golang.org.", searchY = "test-issue19592.y.golang.org.";
    const std::string ip4 = "192.0.2.1", ip6 = "2001:db8::1";
    enum resolveWhichEnum { resolveOK, resolveOpError, resolveServfail, resolveTimeout };
    struct wantErr {
        bool set = false;
        std::string err;
        bool timeout = false, temporary = false, notFound = false;
    };
    auto makeTempError = [](std::string err) { return wantErr{true, err, false, true, false}; };
    auto makeTimeout = [] { return wantErr{true, "i/o timeout", true, true, false}; };
    auto makeNxDomain = [] { return wantErr{true, "no such host", false, false, true}; };
    struct testCase {
        std::string desc;
        std::function<resolveWhichEnum(const dm::Question&)> resolveWhich;
        wantErr wantStrictErr, wantLaxErr;
        std::set<std::string> wantIPs;
    };
    std::vector<testCase> cases = {
        {"No errors", [](const dm::Question&) { return resolveOK; }, {}, {}, {ip4, ip6}},
        {"searchX error fails in strict mode",
         [&](const dm::Question& q) { return q.Name.String() == searchX ? resolveTimeout : resolveOK; },
         makeTimeout(),
         {},
         {ip4, ip6}},
        {"searchX IPv4-only timeout fails in strict mode",
         [&](const dm::Question& q) {
             return q.Name.String() == searchX && q.Type == dm::TypeA ? resolveTimeout : resolveOK;
         },
         makeTimeout(),
         {},
         {ip4, ip6}},
        {"searchX IPv6-only servfail fails in strict mode",
         [&](const dm::Question& q) {
             return q.Name.String() == searchX && q.Type == dm::TypeAAAA ? resolveServfail : resolveOK;
         },
         makeTempError("server misbehaving"),
         {},
         {ip4, ip6}},
        {"searchY error always fails",
         [&](const dm::Question& q) { return q.Name.String() == searchY ? resolveTimeout : resolveOK; },
         makeTimeout(),
         makeNxDomain(),  // this one reaches the "test." FQDN
         {}},
        {"searchY IPv4-only socket error fails in strict mode",
         [&](const dm::Question& q) {
             return q.Name.String() == searchY && q.Type == dm::TypeA ? resolveOpError : resolveOK;
         },
         makeTempError("write: socket on fire"),
         {},
         {ip6}},
        {"searchY IPv6-only timeout fails in strict mode",
         [&](const dm::Question& q) {
             return q.Name.String() == searchY && q.Type == dm::TypeAAAA ? resolveTimeout : resolveOK;
         },
         makeTimeout(),
         {},
         {ip4}},
    };
    for (auto& tt : cases) {
        fakeDNSServer fake{[&](std::string_view, std::string_view, const dm::Message& q, time::Time) {
            switch (tt.resolveWhich(q.Questions[0])) {
            case resolveOK: break;
            case resolveOpError: {
                auto e = make_ptr<OpError>();
                e->Op = string::fromStatic("write");
                e->Err = errors::New("socket on fire");
                return std::pair<dm::Message, error>{dm::Message{}, e};
            }
            case resolveServfail: return ok(reply(q, dm::RCodeServerFailure));
            case resolveTimeout: return std::pair<dm::Message, error>{dm::Message{}, os::ErrDeadlineExceeded};
            }
            std::string qn = q.Questions[0].Name.String();
            if (qn == searchX || qn == name + ".") return ok(reply(q, dm::RCodeNameError));
            if (qn != searchY) return std::pair<dm::Message, error>{dm::Message{}, errors::New("Unexpected Name: " + qn)};
            dm::Message r = reply(q);
            if (q.Questions[0].Type == dm::TypeA) r.Answers = {resource(q.Questions[0].Name, aRes(TestAddr))};
            else if (q.Questions[0].Type == dm::TypeAAAA) r.Answers = {resource(q.Questions[0].Name, aaaaRes(TestAddr6))};
            return ok(r);
        }};
        for (bool strict : {true, false}) {
            SCOPED_TRACE(tt.desc + (strict ? " strict" : " lax"));
            auto r = resolverWith(fake, strict);
            auto [ips, err] = r->LookupIPAddr(context::Background(), name);
            const wantErr& want = strict ? tt.wantStrictErr : tt.wantLaxErr;
            if (want.set) {
                auto* de = errors::As<DNSError>(err);
                ASSERT_TRUE(de) << err;
                EXPECT_EQ(str(de->Err), want.err);
                EXPECT_EQ(str(de->Name), name);
                EXPECT_EQ(str(de->Server), server);
                EXPECT_EQ(de->IsTimeout, want.timeout);
                EXPECT_EQ(de->IsTemporary, want.temporary);
                EXPECT_EQ(de->IsNotFound, want.notFound);
            } else {
                EXPECT_FALSE(err) << err;
            }
            std::set<std::string> gotIPs;
            for (auto& ip : ips) gotIPs.insert(str(ip.String()));
            EXPECT_EQ(gotIPs, want.set ? std::set<std::string>{} : tt.wantIPs);
        }
    }
}

TEST(GoNetDNS, StrictErrorsLookupTXT) {
    resolvConfTest conf;
    conf.writeAndUpdate({"nameserver 192.0.2.53", "search x.golang.org y.golang.org"});
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        std::string qn = q.Questions[0].Name.String();
        if (qn == "test.x.golang.org.") return std::pair<dm::Message, error>{dm::Message{}, os::ErrDeadlineExceeded};
        if (qn == "test.y.golang.org.") return ok(mockTXTResponse(q));
        return std::pair<dm::Message, error>{dm::Message{}, errors::New("Unexpected Name: " + qn)};
    }};
    for (bool strict : {true, false}) {
        auto r = resolverWith(fake, strict);
        auto [p, server, err] = resolverLookup(r.get(), context::Background(), "test", dm::TypeTXT, nullptr);
        if (strict) {
            auto* de = errors::As<DNSError>(err);
            ASSERT_TRUE(de) << err;
            EXPECT_EQ(str(de->Err), "i/o timeout");
            EXPECT_EQ(str(de->Name), "test");
            EXPECT_EQ(str(de->Server), "192.0.2.53:53");
            EXPECT_TRUE(de->IsTimeout);
            EXPECT_TRUE(de->IsTemporary);
        } else {
            EXPECT_FALSE(err) << err;
        }
        auto [a, aerr] = p.AllAnswers();
        if (aerr) a.clear();
        EXPECT_EQ(a.size(), strict ? 0u : 1u) << "strict=" << strict;
    }
}

namespace {

error lookupWithFake(const fakeDNSServer& fake, const std::string& name, dm::Type typ) {
    auto r = resolverWith(fake);
    auto conf = getSystemDNSConfig();
    auto [p, server, err] = tryOneName(r.get(), context::Background(), *conf, name, typ);
    return err;
}

}  // namespace

TEST(GoNetDNS, Issue8434) {
    // Temporary is true for a SERVFAIL.
    error err = lookupWithFake({[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
                                   return ok(reply(q, dm::RCodeServerFailure));
                               }},
                               "golang.org.", dm::TypeALL);
    ASSERT_TRUE(err);
    EXPECT_TRUE(isTemporaryError(err)) << err;
    auto* de = errors::As<DNSError>(err);
    ASSERT_TRUE(de);
    EXPECT_TRUE(de->IsTemporary);
}

TEST(GoNetDNS, IssueNoSuchHostExists) {
    error err = lookupWithFake({[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
                                   return ok(reply(q, dm::RCodeNameError));
                               }},
                               "golang.org.", dm::TypeALL);
    ASSERT_TRUE(err);
    auto* de = errors::As<DNSError>(err);
    ASSERT_TRUE(de);
    EXPECT_TRUE(de->IsNotFound);
}

TEST(GoNetDNS, NoSuchHost) {
    // go.dev/issue/12778, 25336, 27525: NXDOMAIN and empty answers fail
    // fast, as "no such host".
    struct {
        std::string name;
        std::function<dm::Message(const dm::Message&)> f;
    } tests[] = {
        {"NXDOMAIN", [](const dm::Message& q) { return reply(q, dm::RCodeNameError); }},
        {"no answers",
         [](const dm::Message& q) {
             dm::Message r = reply(q);
             r.Header.Authoritative = true;
             return r;
         }},
    };
    for (auto& test : tests) {
        SCOPED_TRACE(test.name);
        int lookups = 0;
        error err = lookupWithFake({[&](std::string_view, std::string_view, const dm::Message& q, time::Time) {
                                       lookups++;
                                       return ok(test.f(q));
                                   }},
                                   ".", dm::TypeALL);
        EXPECT_EQ(lookups, 1);
        auto* de = errors::As<DNSError>(err);
        ASSERT_TRUE(de) << err;
        EXPECT_EQ(str(de->Err), "no such host");
        EXPECT_TRUE(de->IsNotFound);
    }
}

TEST(GoNetDNS, DNSDialTCP) {
    // go.dev/issue/26573: a Conn that isn't a PacketConn is a stream.
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) { return ok(reply(q)); },
                       true};
    auto r = resolverWith(fake);
    auto [p, h, err] = exchange(r.get(), context::Background(), "0.0.0.0",
                                mustQuestion("com.", dm::TypeALL, dm::ClassINET), time::Second, false, false);
    EXPECT_FALSE(err) << err;
}

TEST(GoNetDNS, TXTRecordTwoStrings) {
    // go.dev/issue/27763: the strings of one TXT record are joined.
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        r.Answers = {
            resource(q.Questions[0].Name, make_ptr<dm::TXTResource>(std::vector<std::string>{"string1 ", "string2"})),
            resource(q.Questions[0].Name, make_ptr<dm::TXTResource>(std::vector<std::string>{"onestring"})),
        };
        return ok(r);
    }};
    auto r = resolverWith(fake);
    auto [txt, err] = r->LookupTXT(context::Background(), "golang.org");
    ASSERT_FALSE(err) << err;
    ASSERT_EQ(txt.size(), 2u);
    EXPECT_EQ(str(txt[0]), "string1 string2");
    EXPECT_EQ(str(txt[1]), "onestring");
}

TEST(GoNetDNS, SingleRequestLookup) {
    // go.dev/issue/29644: with single-request, A then AAAA, not together.
    std::atomic<int> firstcalled{0};
    fakeDNSServer fake{[&](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        for (auto& question : q.Questions) {
            if (question.Type == dm::TypeA) {
                if (question.Name.String() == "slowipv4.example.net.") time::Sleep(10ms);
                int want = 0;
                if (!firstcalled.compare_exchange_strong(want, 1)) ADD_FAILURE() << "the A query was received after the AAAA query !";
                r.Answers.push_back(resource(q.Questions[0].Name, aRes(TestAddr)));
            } else if (question.Type == dm::TypeAAAA) {
                int want = 0;
                firstcalled.compare_exchange_strong(want, 2);
                r.Answers.push_back(resource(q.Questions[0].Name, aaaaRes(TestAddr6)));
            }
        }
        return ok(r);
    }};
    auto r = resolverWith(fake);
    resolvConfTest conf;
    conf.writeAndUpdate({"options single-request"});
    for (std::string name : {"hostname.example.net", "slowipv4.example.net"}) {
        firstcalled = 0;
        auto [addrs, err] = r->LookupIPAddr(context::Background(), name);
        EXPECT_FALSE(err) << err;
    }
}

TEST(GoNetDNS, DNSUseTCP) {
    fakeDNSServer fake{[](std::string_view n, std::string_view, const dm::Message& q, time::Time) {
        EXPECT_NE(n, "udp") << "udp protocol was used instead of tcp";
        return ok(reply(q));
    }};
    auto r = resolverWith(fake);
    auto [p, h, err] = exchange(r.get(), context::Background(), "0.0.0.0",
                                mustQuestion("com.", dm::TypeALL, dm::ClassINET), time::Second, true, false);
    EXPECT_FALSE(err) << err;
}

TEST(GoNetDNS, DNSUseTCPTruncated) {
    fakeDNSServer fake{[](std::string_view n, std::string_view, const dm::Message& q, time::Time) {
        EXPECT_NE(n, "udp") << "udp protocol was used instead of tcp";
        dm::Message r = reply(q);
        r.Header.Truncated = true;
        r.Answers = {resource(q.Questions[0].Name, aRes(TestAddr))};
        return ok(r);
    }};
    auto r = resolverWith(fake);
    auto [p, h, err] = exchange(r.get(), context::Background(), "0.0.0.0",
                                mustQuestion("com.", dm::TypeALL, dm::ClassINET), time::Second, true, false);
    ASSERT_FALSE(err) << err;
    auto [a, aerr] = p.AllAnswers();
    ASSERT_FALSE(aerr);
    EXPECT_EQ(a.size(), 1u);
}

TEST(GoNetDNS, PTRandNonPTR) {
    // go.dev/issue/34660: non-PTR answers to a PTR query are ignored.
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        r.Answers = {
            resource(q.Questions[0].Name, make_ptr<dm::PTRResource>(dm::MustNewName("golang.org."))),
            resource(q.Questions[0].Name, make_ptr<dm::TXTResource>(std::vector<std::string>{"PTR 8 6 60 ..."})),
        };
        return ok(r);
    }};
    auto r = resolverWith(fake);
    auto [names, err] = r->LookupAddr(context::Background(), "192.0.2.123");
    ASSERT_FALSE(err) << err;
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(str(names[0]), "golang.org.");
}

TEST(GoNetDNS, NullMX) {
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        r.Answers = {resource(q.Questions[0].Name, make_ptr<dm::MXResource>(0, dm::MustNewName(".")))};
        return ok(r);
    }};
    auto r = resolverWith(fake);
    auto [rrset, err] = r->LookupMX(context::Background(), "golang.org");
    ASSERT_FALSE(err) << err;
    ASSERT_EQ(rrset.size(), 1u);
    EXPECT_EQ(str(rrset[0]->Host), ".");
    EXPECT_EQ(rrset[0]->Pref, 0);
}

TEST(GoNetDNS, RootNS) {
    // go.dev/issue/45715.
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        dm::Message r = reply(q);
        r.Answers = {resource(q.Questions[0].Name, make_ptr<dm::NSResource>(dm::MustNewName("i.root-servers.net.")))};
        return ok(r);
    }};
    auto r = resolverWith(fake);
    auto [rrset, err] = r->LookupNS(context::Background(), ".");
    ASSERT_FALSE(err) << err;
    ASSERT_EQ(rrset.size(), 1u);
    EXPECT_EQ(str(rrset[0]->Host), "i.root-servers.net.");
}

namespace {

void testGoLookupIPCNAMEOrderHostsAliases(hostLookupOrder mode, const std::string& lookup,
                                          const std::string& lookupRes) {
    fakeDNSServer fake{[mode](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        EXPECT_EQ(mode, hostLookupDNSFiles) << "received unexpected DNS query";
        dm::Message r = reply(q);
        r.Questions = {q.Questions[0]};
        return ok(r);
    }};
    auto r = resolverWith(fake);
    for (auto& in : {lookup, absDomainName(lookup), lowerASCII(lookup), upper(lookup)}) {
        auto [addrs, res, err] = goLookupIPCNAMEOrder(r.get(), context::Background(), "ip", in, mode, nullptr);
        EXPECT_FALSE(err) << err;
        EXPECT_EQ(res.String(), lookupRes) << "goLookupIPCNAMEOrder(" << in << ")";
    }
}

}  // namespace

TEST(GoNetDNS, GoLookupIPCNAMEOrderHostsAliasesFilesOnlyMode) {
    hostsFileGuard g;
    hostsFilePath = testdata("aliases");
    for (auto& v : lookupStaticHostAliasesTest) {
        testGoLookupIPCNAMEOrderHostsAliases(hostLookupFiles, v.lookup, absDomainName(v.res));
    }
}

TEST(GoNetDNS, GoLookupIPCNAMEOrderHostsAliasesFilesDNSMode) {
    hostsFileGuard g;
    hostsFilePath = testdata("aliases");
    for (auto& v : lookupStaticHostAliasesTest) {
        testGoLookupIPCNAMEOrderHostsAliases(hostLookupFilesDNS, v.lookup, absDomainName(v.res));
    }
}

TEST(GoNetDNS, GoLookupIPCNAMEOrderHostsAliasesDNSFilesMode) {
    hostsFileGuard g;
    hostsFilePath = testdata("aliases");
    testGoLookupIPCNAMEOrderHostsAliases(hostLookupDNSFiles, "invalid.invalid", absDomainName("invalid.test"));
}

TEST(GoNetDNS, DNSPacketSize) {
    for (bool disable : {false, true}) {
        SCOPED_TRACE(disable ? "disabled" : "enabled");
        fakeDNSServer fake{[disable](std::string_view, std::string_view, const dm::Message& q, time::Time) {
            if (disable) {
                EXPECT_TRUE(q.Additionals.empty()) << "unexpected additional record";
            } else if (q.Additionals.empty()) {
                ADD_FAILURE() << "missing EDNS record";
            } else {
                auto* opt = dynamic_cast<dm::OPTResource*>(q.Additionals[0].Body.get());
                EXPECT_TRUE(opt);
                if (opt) { EXPECT_TRUE(opt->Options.empty()); }
                EXPECT_EQ(int(q.Additionals[0].Header.Class), 1232);
            }
            dm::Message r = reply(q);
            if (q.Questions[0].Type == dm::TypeA) r.Answers = {resource(q.Questions[0].Name, aRes(TestAddr))};
            return ok(r);
        }};
        const char* old = std::getenv("GODEBUG");
        std::string saved = old ? old : "";
        if (disable) ::setenv("GODEBUG", "netedns0=0", 1);
        auto r = resolverWith(fake);
        auto [addrs, err] = r->LookupIPAddr(context::Background(), "go.dev");
        EXPECT_FALSE(err) << err;
        if (old) ::setenv("GODEBUG", saved.c_str(), 1);
        else ::unsetenv("GODEBUG");
    }
}

TEST(GoNetDNS, DNSTrustAD) {
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        std::string qn = q.Questions[0].Name.String();
        if (qn == "notrustad.go.dev.") { EXPECT_FALSE(q.Header.AuthenticData) << "unexpected AD bit"; }
        if (qn == "trustad.go.dev.") { EXPECT_TRUE(q.Header.AuthenticData) << "expected AD bit"; }
        dm::Message r = reply(q);
        if (q.Questions[0].Type == dm::TypeA) r.Answers = {resource(q.Questions[0].Name, aRes(TestAddr))};
        return ok(r);
    }};
    auto r = resolverWith(fake);
    resolvConfTest conf;
    conf.writeAndUpdate({"nameserver 127.0.0.1"});
    EXPECT_FALSE(r->LookupIPAddr(context::Background(), "notrustad.go.dev").second);
    conf.writeAndUpdate({"nameserver 127.0.0.1", "options trust-ad"});
    EXPECT_FALSE(r->LookupIPAddr(context::Background(), "trustad.go.dev").second);
}

TEST(GoNetDNS, DNSConfigNoReload) {
    auto r = make_ptr<Resolver>();
    r->Dial = [](const ptr<context::Context>& ctx, std::string_view network, std::string_view address) {
        if (address != "192.0.2.1:53") {
            return std::pair<ptr<Conn>, error>{nullptr, errors::New("configuration unexpectedly changed")};
        }
        return fakeDNSServerSuccessful.DialContext(ctx, network, address);
    };
    resolvConfTest conf;
    conf.writeAndUpdate({"nameserver 192.0.2.1", "options no-reload"});
    ASSERT_FALSE(r->LookupHost(context::Background(), "go.dev").second);
    conf.write({"nameserver 192.0.2.200"});
    ASSERT_FALSE(r->LookupHost(context::Background(), "go.dev").second);
}

TEST(GoNetDNS, LookupOrderFilesNoSuchHost) {
    hostsFileGuard g;
    nssForceUpdate(parseNSSConf("hosts: files"));
    resolvConfTest conf;
    conf.writeAndUpdate({});
    std::string tmpFile = conf.dir + "/hosts";
    ASSERT_FALSE(os::WriteFile(tmpFile, std::string_view(), 0660));
    hostsFilePath = tmpFile;
    const std::string testName = "test.invalid";
    auto [order, dc] = hostLookupOrderFor(testName);
    EXPECT_EQ(order, hostLookupFiles);
    auto check = [&](const char* what, const error& err) {
        ASSERT_TRUE(err) << what << ": unexpected success";
        auto* de = errors::As<DNSError>(err);
        ASSERT_TRUE(de) << err;
        EXPECT_EQ(str(de->Err), "no such host") << what;
        EXPECT_EQ(str(de->Name), testName) << what;
        EXPECT_TRUE(de->IsNotFound) << what;
        EXPECT_TRUE(de->Server.empty()) << what;
        EXPECT_FALSE(de->IsTimeout || de->IsTemporary) << what;
    };
    check("Host", DefaultResolver->LookupHost(context::Background(), testName).second);
    check("IP", DefaultResolver->LookupIP(context::Background(), "ip", testName).second);
    check("IPAddr", DefaultResolver->LookupIPAddr(context::Background(), testName).second);
    check("NetIP", DefaultResolver->LookupNetIP(context::Background(), "ip", testName).second);
    nssForceUpdate(nullptr);
}

TEST(GoNetDNS, ExtendedRCode) {
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        auto fraudSuccessCode = dm::RCode(dm::RCodeSuccess | 1 << 10);
        dm::Resource opt;
        opt.Header.SetEDNS0(1232, fraudSuccessCode, false);
        opt.Body = make_ptr<dm::OPTResource>();
        dm::Message r = reply(q, fraudSuccessCode);
        r.Questions = {q.Questions[0]};
        r.Additionals = {opt};
        return ok(r);
    }};
    auto r = resolverWith(fake);
    auto [p, server, err] = tryOneName(r.get(), context::Background(), *getSystemDNSConfig(), "go.dev.", dm::TypeA);
    auto* de = errors::As<DNSError>(err);
    ASSERT_TRUE(de) << err;
    EXPECT_EQ(str(de->Err), "server misbehaving");
}

TEST(GoNetDNS, EmptyResolvConfReplacedWithConfHaingNameservers) {
    // The default nameservers make every lookup check resolv.conf again.
    resolvConfTest conf;
    conf.writeAndUpdate({"# empty resolv.conf file"});
    EXPECT_TRUE(getSystemDNSConfig()->defaultNS);
    conf.writeAndUpdate({"nameserver 192.0.2.1"});
    EXPECT_FALSE(getSystemDNSConfig()->defaultNS);
}

// go++'s own: a lookup gives up when its context is done, with an error
// that is the context's.
TEST(GoNetDNS, LookupContextCanceled) {
    fakeDNSServer fake{[](std::string_view, std::string_view, const dm::Message& q, time::Time) {
        time::Sleep(200ms);
        return ok(reply(q));
    }};
    auto r = resolverWith(fake);
    resolvConfTest conf;
    conf.writeAndUpdate({"nameserver 192.0.2.1"});
    auto [ctx, cancel] = context::WithTimeout(context::Background(), 20 * time::Millisecond);
    auto start = time::Now();
    auto [addrs, err] = r->LookupIPAddr(ctx, "slow.example");
    EXPECT_LT(time::Since(start).Milliseconds(), 150);
    auto* de = errors::As<DNSError>(err);
    ASSERT_TRUE(de) << err;
    EXPECT_TRUE(de->IsTimeout);
    EXPECT_TRUE(errors::Is(err, context::DeadlineExceeded)) << err;
    cancel();
}

// ---------------------------------------------------------------------------
// lookup_test.go

namespace {

// Go's mustHaveExternalNetwork: these tests query the Internet's DNS.
bool haveExternalNetwork() {
    static bool ok = [] {
        auto [addrs, err] = LookupHost("go.dev");
        return !err && !addrs.empty();
    }();
    return ok;
}

#define MUST_HAVE_EXTERNAL_NETWORK() \
    if (!haveExternalNetwork()) GTEST_SKIP() << "no external network"

bool hasSuffixFold(std::string_view s, std::string_view suffix) { return stringsHasSuffixFold(s, suffix); }

}  // namespace

TEST(GoNetDNS, LookupGoogleSRV) {
    MUST_HAVE_EXTERNAL_NETWORK();
    struct {
        std::string service, proto, name, cname, target;
    } tests[] = {
        {"ldap", "tcp", "google.com", "google.com.", "google.com."},
        {"ldap", "tcp", "google.com.", "google.com.", "google.com."},
        // Non-standard back door.
        {"", "", "_ldap._tcp.google.com", "google.com.", "google.com."},
        {"", "", "_ldap._tcp.google.com.", "google.com.", "google.com."},
    };
    for (auto& tt : tests) {
        auto [cname, srvs, err] = LookupSRV(tt.service, tt.proto, tt.name);
        ASSERT_FALSE(err) << err;
        EXPECT_FALSE(srvs.empty()) << "got no record";
        EXPECT_TRUE(hasSuffixFold(cname.view(), tt.cname)) << cname.view();
        for (auto& srv : srvs) EXPECT_TRUE(hasSuffixFold(srv->Target.view(), tt.target)) << srv->Target.view();
    }
}

TEST(GoNetDNS, LookupGmailMX) {
    MUST_HAVE_EXTERNAL_NETWORK();
    for (std::string name : {"gmail.com", "gmail.com."}) {
        auto [mxs, err] = LookupMX(name);
        ASSERT_FALSE(err) << err;
        EXPECT_FALSE(mxs.empty()) << "got no record";
        for (auto& mx : mxs) EXPECT_TRUE(hasSuffixFold(mx->Host.view(), "google.com.")) << mx->Host.view();
        for (std::size_t i = 1; i < mxs.size(); i++) EXPECT_LE(mxs[i - 1]->Pref, mxs[i]->Pref) << "not sorted";
    }
}

TEST(GoNetDNS, LookupGmailNS) {
    MUST_HAVE_EXTERNAL_NETWORK();
    for (std::string name : {"gmail.com", "gmail.com."}) {
        auto [nss, err] = LookupNS(name);
        ASSERT_FALSE(err) << err;
        EXPECT_FALSE(nss.empty()) << "got no record";
        for (auto& ns : nss) EXPECT_TRUE(hasSuffixFold(ns->Host.view(), "google.com.")) << ns->Host.view();
    }
}

TEST(GoNetDNS, LookupGmailTXT) {
    MUST_HAVE_EXTERNAL_NETWORK();
    for (std::string name : {"gmail.com", "gmail.com."}) {
        auto [txts, err] = LookupTXT(name);
        ASSERT_FALSE(err) << err;
        bool found = false;
        for (auto& txt : txts) {
            if (txt.view().find("spf") != std::string_view::npos) found = true;
        }
        EXPECT_TRUE(found) << "got no record containing spf";
    }
}

TEST(GoNetDNS, LookupGooglePublicDNSAddr) {
    MUST_HAVE_EXTERNAL_NETWORK();
    for (std::string ip : {"8.8.8.8", "8.8.4.4", "2001:4860:4860::8888", "2001:4860:4860::8844"}) {
        auto [names, err] = LookupAddr(ip);
        ASSERT_FALSE(err) << ip << ": " << err;
        EXPECT_FALSE(names.empty()) << "got no record";
        for (auto& name : names) {
            EXPECT_TRUE(hasSuffixFold(name.view(), ".google.com.") || hasSuffixFold(name.view(), ".google."))
                << name.view();
        }
    }
}

TEST(GoNetDNS, LookupCNAME) {
    MUST_HAVE_EXTERNAL_NETWORK();
    struct {
        std::string name, cname;
    } tests[] = {
        {"www.golang.org", "golang.org."},
        {"www.golang.org.", "golang.org."},
        {"www.google.com", "google.com."},
        {"google.com", "google.com."},
        {"cname-to-txt.go4.org", "test-txt-record.go4.org."},
    };
    for (auto& tt : tests) {
        auto [cname, err] = LookupCNAME(tt.name);
        ASSERT_FALSE(err) << tt.name << ": " << err;
        EXPECT_TRUE(hasSuffixFold(cname.view(), tt.cname)) << tt.name << ": " << cname.view();
    }
}

TEST(GoNetDNS, LookupGoogleHost) {
    MUST_HAVE_EXTERNAL_NETWORK();
    for (std::string name : {"google.com", "google.com."}) {
        auto [addrs, err] = LookupHost(name);
        ASSERT_FALSE(err) << err;
        EXPECT_FALSE(addrs.empty()) << "got no record";
        for (auto& addr : addrs) EXPECT_FALSE(ParseIP(addr.view()).empty()) << addr.view();
    }
}

TEST(GoNetDNS, LookupGoogleIP) {
    MUST_HAVE_EXTERNAL_NETWORK();
    for (std::string name : {"google.com", "google.com."}) {
        auto [ips, err] = LookupIP(name);
        ASSERT_FALSE(err) << err;
        EXPECT_FALSE(ips.empty()) << "got no record";
    }
}

TEST(GoNetDNS, LookupNoSuchHost) {
    MUST_HAVE_EXTERNAL_NETWORK();
    const std::string testNXDOMAIN = "invalid.invalid.", testNODATA = "_ldap._tcp.google.com.";
    struct {
        std::string name;
        std::function<error()> query;
    } tests[] = {
        {"LookupCNAME NXDOMAIN", [&] { return LookupCNAME(testNXDOMAIN).second; }},
        {"LookupHost NXDOMAIN", [&] { return LookupHost(testNXDOMAIN).second; }},
        {"LookupHost NODATA", [&] { return LookupHost(testNODATA).second; }},
        {"LookupMX NXDOMAIN", [&] { return LookupMX(testNXDOMAIN).second; }},
        {"LookupMX NODATA", [&] { return LookupMX(testNODATA).second; }},
        {"LookupNS NXDOMAIN", [&] { return LookupNS(testNXDOMAIN).second; }},
        {"LookupNS NODATA", [&] { return LookupNS(testNODATA).second; }},
        {"LookupSRV NXDOMAIN", [&] { return std::get<2>(LookupSRV("unknown", "tcp", testNXDOMAIN)); }},
        {"LookupTXT NXDOMAIN", [&] { return LookupTXT(testNXDOMAIN).second; }},
        {"LookupTXT NODATA", [&] { return LookupTXT(testNODATA).second; }},
    };
    for (auto& v : tests) {
        error err = v.query();
        ASSERT_TRUE(err) << v.name << ": unexpected success";
        auto* de = errors::As<DNSError>(err);
        ASSERT_TRUE(de) << v.name << ": " << err;
        EXPECT_TRUE(de->IsNotFound) << v.name;
        EXPECT_EQ(str(de->Err), "no such host") << v.name;
    }
}

TEST(GoNetDNS, LookupNullByte) {
    MUST_HAVE_EXTERNAL_NETWORK();
    LookupHost(std::string_view("foo\0bar", 7));  // check that it doesn't crash
}

TEST(GoNetDNS, LookupNonLDH) {
    // Names that aren't letters, digits and hyphens are "not found", not
    // "invalid".
    auto [addrs, err] = LookupHost("!!!.###.bogus..domain.");
    ASSERT_TRUE(err) << "lookup succeeded";
    EXPECT_TRUE(err.Error().view().ends_with("no such host")) << err;
    auto* de = errors::As<DNSError>(err);
    ASSERT_TRUE(de);
    EXPECT_TRUE(de->IsNotFound);
}

TEST(GoNetDNS, LookupPortNotFound) {
    auto [port, err] = LookupPort("udp", "_-unknown-service-");
    auto* de = errors::As<DNSError>(err);
    EXPECT_TRUE(de && de->IsNotFound) << err;
}

TEST(GoNetDNS, LookupPortDifferentNetwork) {
    auto [port, err] = LookupPort("udp", "submissions");
    auto* de = errors::As<DNSError>(err);
    EXPECT_TRUE(de && de->IsNotFound) << err;
}

TEST(GoNetDNS, LookupPortEmptyNetworkString) { EXPECT_FALSE(LookupPort("", "submissions").second); }

TEST(GoNetDNS, LookupPortIPNetworkString) { EXPECT_FALSE(LookupPort("ip", "submissions").second); }

TEST(GoNetDNS, LookupProtocol_Minimal) {
    struct {
        std::string name;
        int want;
    } tests[] = {{"tcp", 6}, {"TcP", 6}, {"icmp", 1}, {"igmp", 2}, {"udp", 17}, {"ipv6-icmp", 58}};
    for (auto& tt : tests) {
        auto [got, err] = lookupProtocol(context::Background(), tt.name);
        EXPECT_EQ(got, tt.want) << tt.name;
        EXPECT_FALSE(err) << err;
    }
}

TEST(GoNetDNS, IPVersion) {
    struct {
        std::string network;
        char version;
    } tests[] = {{"tcp", 0},  {"tcp4", '4'}, {"tcp6", '6'}, {"udp", 0},  {"udp4", '4'},
                 {"udp6", '6'}, {"ip", 0},    {"ip4", '4'},  {"ip6", '6'}, {"ip7", 0},  {"", 0}};
    for (auto& tt : tests) EXPECT_EQ(ipVersion(tt.network), tt.version) << tt.network;
}

TEST(GoNetDNS, ResolverLookupIPWithEmptyHost) {
    auto [ips, err] = DefaultResolver->LookupIP(context::Background(), "ip", "");
    ASSERT_TRUE(err);
    EXPECT_TRUE(err.Error().view().ends_with("no such host")) << err;
}

TEST(GoNetDNS, DNSErrorUnwrap) {
    auto rDeadlineExceeded = make_ptr<Resolver>();
    rDeadlineExceeded->Dial = [](const ptr<context::Context>&, std::string_view, std::string_view) {
        return std::pair<ptr<Conn>, error>{nullptr, context::DeadlineExceeded};
    };
    auto rCancelled = make_ptr<Resolver>();
    rCancelled->Dial = [](const ptr<context::Context>&, std::string_view, std::string_view) {
        return std::pair<ptr<Conn>, error>{nullptr, context::Canceled};
    };
    EXPECT_TRUE(errors::Is(rDeadlineExceeded->LookupHost(context::Background(), "test.go.dev").second,
                           context::DeadlineExceeded));
    EXPECT_TRUE(errors::Is(rCancelled->LookupHost(context::Background(), "test.go.dev").second, context::Canceled));
    auto [ctx, cancel] = context::WithCancel(context::Background());
    cancel();
    EXPECT_TRUE(errors::Is(DefaultResolver->LookupHost(ctx, "text.go.dev").second, context::Canceled));
}

// Dialing a name goes through the resolver too.
TEST(GoNetDNS, DialByName) {
    MUST_HAVE_EXTERNAL_NETWORK();
    net::Dialer d{.Timeout = 10 * time::Second};
    auto [c, err] = d.Dial("tcp", "go.dev:443");
    ASSERT_FALSE(err) << err;
    c->Close();
}
