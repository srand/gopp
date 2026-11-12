// IP socket addresses and their resolution, after Go's net/ipsock.go,
// tcpsock.go, udpsock.go, lookup.go and port.go.
#include "ipsock.hpp"

#include <gopp/runtime.hpp>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace gopp::net {

namespace {

string ipEmptyString(const IP& ip) { return ip.empty() ? string() : ip.String(); }

string ip_addr_string(const IP& ip, int port, const string& zone) {
    string host = ipEmptyString(ip);
    if (!zone.empty()) host = host + "%" + zone;
    return JoinHostPort(host.view(), std::to_string(port));
}

// Go's parsePort: a decimal port, or whether the service needs a lookup.
std::pair<int, bool> parsePort(std::string_view service) {
    if (service.empty()) return {0, false};
    constexpr uint32_t max = 0xffffffffu, cutoff = 1u << 30;
    bool neg = false;
    if (service[0] == '+') {
        service.remove_prefix(1);
    } else if (service[0] == '-') {
        neg = true;
        service.remove_prefix(1);
    }
    uint32_t n = 0;
    for (char c : service) {
        if (c < '0' || c > '9') return {0, true};
        uint32_t d = uint32_t(c - '0');
        if (n >= cutoff) {
            n = max;
            break;
        }
        n *= 10;
        uint32_t nn = n + d;
        if (nn < n) {
            n = max;
            break;
        }
        n = nn;
    }
    int port;
    if (!neg && n >= cutoff) port = int(cutoff - 1);
    else if (neg && n > cutoff) port = int(cutoff);
    else port = int(n);
    return {neg ? -port : port, false};
}

// Go's services, completed from /etc/services on first use, as its
// pure-Go resolver does (port_unix.go).
std::map<std::string, std::map<std::string, int>>& services() {
    static std::map<std::string, std::map<std::string, int>> m = [] {
        std::map<std::string, std::map<std::string, int>> s = {
            {"udp", {{"domain", 53}}},
            {"tcp",
             {{"ftp", 21}, {"ftps", 990}, {"gopher", 70}, {"http", 80}, {"https", 443}, {"imap2", 143},
              {"imap3", 220}, {"imaps", 993}, {"pop3", 110}, {"pop3s", 995}, {"smtp", 25}, {"submissions", 465},
              {"ssh", 22}, {"telnet", 23}}},
        };
        runtime::blocking([&] {
            std::ifstream f("/etc/services");
            std::string line;
            while (std::getline(f, line)) {
                // "http 80/tcp www # comment"
                if (auto h = line.find('#'); h != line.npos) line.resize(h);
                std::istringstream in(line);
                std::string name, portnet, alias;
                if (!(in >> name >> portnet)) continue;
                auto slash = portnet.find('/');
                if (slash == portnet.npos) continue;
                auto [port, needs] = parsePort(std::string_view(portnet).substr(0, slash));
                if (needs || port <= 0) continue;
                auto& m = s[portnet.substr(slash + 1)];
                m.emplace(name, port);
                while (in >> alias) m.emplace(alias, port);
            }
            return 0;
        });
        return s;
    }();
    return m;
}

std::pair<int, error> lookupPortMapWithNetwork(const std::string& network, std::string_view errNetwork,
                                               std::string_view service) {
    auto& all = services();
    auto it = all.find(network);
    if (it == all.end()) {
        auto e = make_ptr<DNSError>();
        e->Err = string::from_static("unknown network");
        e->Name = string(errNetwork) + "/" + string(service);
        return {0, e};
    }
    std::string lower(service);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (auto p = it->second.find(lower); p != it->second.end()) return {p->second, nullptr};
    auto e = make_ptr<DNSError>();
    e->Err = string::from_static("unknown port");
    e->Name = string(errNetwork) + "/" + string(service);
    e->IsNotFound = true;
    return {0, e};
}

struct ip_zone {
    IP ip;
    string zone;
};

// Go's lookupIPAddr: an address literal (IPv6 with a zone too), or the
// addresses of a name.
std::pair<std::vector<ip_zone>, error> lookupIPAddr(std::string_view network, std::string_view host) {
    if (IP ip = ParseIP(host); !ip.empty()) return {{{ip, {}}}, nullptr};
    if (auto pct = host.rfind('%'); pct != host.npos && pct > 0 && host.substr(0, pct).find(':') != host.npos &&
                                    pct + 1 < host.size()) {
        if (IP ip = ParseIP(host.substr(0, pct)); !ip.empty() && ip.To4().empty()) {
            return {{{ip, string(host.substr(pct + 1))}}, nullptr};
        }
    }
    if (host == "localhost") return {{{IPv4(127, 0, 0, 1), {}}, {IPv6loopback, {}}}, nullptr};

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    if (!network.empty() && network.back() == '4') hints.ai_family = AF_INET;
    if (!network.empty() && network.back() == '6') hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;  // one entry per address
    addrinfo* res = nullptr;
    std::string h(host);
    int rc = runtime::blocking([&] { return getaddrinfo(h.c_str(), nullptr, &hints, &res); });
    if (rc != 0) {
        auto e = make_ptr<DNSError>();
        e->Err = rc == EAI_NONAME || rc == EAI_NODATA ? string::from_static("no such host") : string(gai_strerror(rc));
        e->Name = string(host);
        e->IsNotFound = rc == EAI_NONAME || rc == EAI_NODATA;
        e->IsTemporary = rc == EAI_AGAIN;
        return {{}, e};
    }
    std::vector<ip_zone> out;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            auto* in = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
            out.push_back({IP(slice<byte>::of(std::span(reinterpret_cast<const byte*>(&in->sin_addr), 4))), {}});
        } else if (ai->ai_family == AF_INET6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(ai->ai_addr);
            out.push_back({IP(slice<byte>::of(std::span(reinterpret_cast<const byte*>(&in6->sin6_addr), 16))), {}});
        }
    }
    freeaddrinfo(res);
    return {out, nullptr};
}

bool is_ipv4(const Addr* a) {
    ip_port ip = ip_port_of(a);
    return ip.ok && !ip.ip.To4().empty();
}

bool supports_ipv6() {
    static bool ok = [] {
        int s = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) return false;
        ::close(s);
        return true;
    }();
    return ok;
}

int addr_family(const ip_port& a) {
    if (a.ip.empty() || a.ip.size() <= IPv4len) return AF_INET;
    if (!a.ip.To4().empty()) return AF_INET;
    return AF_INET6;
}

}  // namespace

string TCPAddr::String() const { return ip_addr_string(IP, Port, Zone); }
string UDPAddr::String() const { return ip_addr_string(IP, Port, Zone); }

string AddrError::Error() const {
    if (Addr.empty()) return Err;
    return "address " + Addr + ": " + Err;
}

string DNSError::Error() const {
    string s = "lookup " + Name;
    if (!Server.empty()) s = s + " on " + Server;
    return s + ": " + Err;
}

std::tuple<string, string, error> SplitHostPort(std::string_view hostport) {
    auto addrErr = [&](std::string_view why) {
        return std::tuple<string, string, error>{{}, {}, make_ptr<AddrError>(string(why), string(hostport))};
    };
    std::size_t j = 0, k = 0;
    auto i = hostport.rfind(':');
    if (i == hostport.npos) return addrErr("missing port in address");
    std::string_view host;
    if (hostport[0] == '[') {
        auto end = hostport.find(']');
        if (end == hostport.npos) return addrErr("missing ']' in address");
        if (end + 1 == hostport.size()) return addrErr("missing port in address");
        if (end + 1 != i) {
            if (hostport[end + 1] == ':') return addrErr("too many colons in address");
            return addrErr("missing port in address");
        }
        host = hostport.substr(1, end - 1);
        j = 1;
        k = end + 1;
    } else {
        host = hostport.substr(0, i);
        if (host.find(':') != host.npos) return addrErr("too many colons in address");
    }
    if (hostport.substr(j).find('[') != hostport.npos) return addrErr("unexpected '[' in address");
    if (hostport.substr(k).find(']') != hostport.npos) return addrErr("unexpected ']' in address");
    return {string(host), string(hostport.substr(i + 1)), nullptr};
}

string JoinHostPort(std::string_view host, std::string_view port) {
    if (host.find(':') != host.npos) return string("[") + string(host) + "]:" + string(port);
    return string(host) + ":" + string(port);
}

std::pair<int, error> LookupPort(std::string_view network, std::string_view service) {
    auto [port, needsLookup] = parsePort(service);
    if (needsLookup) {
        std::string net;
        if (network == "tcp" || network == "tcp4" || network == "tcp6") net = "tcp";
        else if (network == "udp" || network == "udp4" || network == "udp6") net = "udp";
        else if (network == "ip" || network.empty()) net = "ip";
        else return {0, make_ptr<AddrError>(string::from_static("unknown network"), string(network))};
        error err;
        if (net == "ip") {
            std::tie(port, err) = lookupPortMapWithNetwork("tcp", "ip", service);
            if (err) std::tie(port, err) = lookupPortMapWithNetwork("udp", "ip", service);
        } else {
            std::tie(port, err) = lookupPortMapWithNetwork(net, net, service);
        }
        if (err) return {0, err};
    }
    if (port < 0 || port > 65535) return {0, make_ptr<AddrError>(string::from_static("invalid port"), string(service))};
    return {port, nullptr};
}

ip_port ip_port_of(const Addr* a) {
    if (auto* t = dynamic_cast<const TCPAddr*>(a)) return {t->IP, t->Port, t->Zone, true};
    if (auto* u = dynamic_cast<const UDPAddr*>(a)) return {u->IP, u->Port, u->Zone, true};
    return {};
}

std::pair<addr_list, error> internet_addr_list(std::string_view net, std::string_view addr) {
    string host, port;
    int portnum = 0;
    bool udp;
    if (net == "tcp" || net == "tcp4" || net == "tcp6") udp = false;
    else if (net == "udp" || net == "udp4" || net == "udp6") udp = true;
    else return {{}, make_ptr<UnknownNetworkError>(string(net))};
    if (!addr.empty()) {
        error err;
        std::tie(host, port, err) = SplitHostPort(addr);
        if (err) return {{}, err};
        std::tie(portnum, err) = LookupPort(net, port.view());
        if (err) return {{}, err};
    }
    auto inetaddr = [&](IP ip, string zone) -> ptr<Addr> {
        if (udp) return make_ptr<UDPAddr>(std::move(ip), portnum, std::move(zone));
        return make_ptr<TCPAddr>(std::move(ip), portnum, std::move(zone));
    };
    if (host.empty()) return {{inetaddr({}, {})}, nullptr};
    auto [ips, err] = lookupIPAddr(net, host.view());
    if (err) return {{}, err};
    // Go 1.0: an IPv6 wildcard listens on IPv4 too.
    if (ips.size() == 1 && ips[0].ip.Equal(IPv6unspecified)) ips.push_back({IPv4zero, {}});
    addr_list out;
    for (auto& ip : ips) {
        if (net.back() == '4' && ip.ip.To4().empty()) continue;
        if (net.back() == '6' && (ip.ip.size() != IPv6len || !ip.ip.To4().empty())) continue;
        out.push_back(inetaddr(ip.ip, ip.zone));
    }
    if (out.empty()) return {{}, make_ptr<AddrError>(string::from_static("no suitable address found"), host)};
    return {out, nullptr};
}

ptr<Addr> first_ipv4(const addr_list& addrs) {
    for (auto& a : addrs) {
        if (is_ipv4(a.get())) return a;
    }
    return addrs.front();
}

int favorite_family(std::string_view network, const Addr* laddr, const Addr* raddr, bool listen, bool* ipv6only) {
    *ipv6only = false;
    if (!network.empty() && network.back() == '4') return AF_INET;
    if (!network.empty() && network.back() == '6') {
        *ipv6only = true;
        return AF_INET6;
    }
    ip_port la = laddr ? ip_port_of(laddr) : ip_port{};
    ip_port ra = raddr ? ip_port_of(raddr) : ip_port{};
    if (listen && (!laddr || la.ip.empty() || la.ip.IsUnspecified())) {
        // A wildcard: a dual-stack socket listens on IPv4 and IPv6.
        if (supports_ipv6()) return AF_INET6;
        if (!laddr) return AF_INET;
        return addr_family(la);
    }
    if ((!laddr || addr_family(la) == AF_INET) && (!raddr || addr_family(ra) == AF_INET)) return AF_INET;
    return AF_INET6;
}

std::pair<ptr<TCPAddr>, error> ResolveTCPAddr(std::string_view network, std::string_view address) {
    if (network.empty()) network = "tcp";  // Go 1.0
    else if (network != "tcp" && network != "tcp4" && network != "tcp6") return {nullptr, make_ptr<UnknownNetworkError>(string(network))};
    auto [addrs, err] = internet_addr_list(network, address);
    if (err) return {nullptr, err};
    // Go's forResolve: an IPv6 address if the literal has '['.
    bool want6 = network == "tcp" && address.find('[') != address.npos;
    ptr<Addr> a = addrs.front();
    for (auto& x : addrs) {
        if (is_ipv4(x.get()) != want6) {
            a = x;
            break;
        }
    }
    return {ptr<TCPAddr>(dynamic_cast<TCPAddr*>(a.get())), nullptr};
}

std::pair<ptr<UDPAddr>, error> ResolveUDPAddr(std::string_view network, std::string_view address) {
    if (network.empty()) network = "udp";  // Go 1.0
    else if (network != "udp" && network != "udp4" && network != "udp6") return {nullptr, make_ptr<UnknownNetworkError>(string(network))};
    auto [addrs, err] = internet_addr_list(network, address);
    if (err) return {nullptr, err};
    bool want6 = network == "udp" && address.find('[') != address.npos;
    ptr<Addr> a = addrs.front();
    for (auto& x : addrs) {
        if (is_ipv4(x.get()) != want6) {
            a = x;
            break;
        }
    }
    return {ptr<UDPAddr>(dynamic_cast<UDPAddr*>(a.get())), nullptr};
}

}  // namespace gopp::net
