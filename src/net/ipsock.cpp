// IP socket addresses, after Go's net/ipsock.go, tcpsock.go, udpsock.go,
// lookup.go (LookupPort) and port.go.
#include "dial.hpp"
#include "dnsclient.hpp"
#include "ipsock.hpp"

#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

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
using services_map = std::map<std::string, std::map<std::string, int>>;

services_map load_services() {
    services_map s = {
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
}

services_map& services() {
    // Loaded under a sync::Once, not by the static's initializer: loading
    // parks the fiber, and fibers waiting on a static's guard block their
    // threads, and with them the Ps the loading fiber needs to finish.
    struct table {
        sync::Once once;
        services_map m;
    };
    static auto* t = new table;
    t->once.Do([] { t->m = load_services(); });
    return t->m;
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
    if (auto* i = dynamic_cast<const IPAddr*>(a)) return {i->IP, 0, i->Zone, true};
    return {};
}

std::pair<ptr<TCPAddr>, error> ResolveTCPAddr(std::string_view network, std::string_view address) {
    if (network.empty()) network = "tcp";  // a hint wildcard for Go 1.0 undocumented behavior
    else if (network != "tcp" && network != "tcp4" && network != "tcp6") return {nullptr, make_ptr<UnknownNetworkError>(string(network))};
    auto [addrs, err] = internetAddrList(nullptr, context::Background(), network, address);
    if (err) return {nullptr, err};
    ptr<Addr> a = forResolve(addrs, network, address);
    return {ptr<TCPAddr>(dynamic_cast<TCPAddr*>(a.get())), nullptr};
}

std::pair<ptr<UDPAddr>, error> ResolveUDPAddr(std::string_view network, std::string_view address) {
    if (network.empty()) network = "udp";  // a hint wildcard for Go 1.0 undocumented behavior
    else if (network != "udp" && network != "udp4" && network != "udp6") return {nullptr, make_ptr<UnknownNetworkError>(string(network))};
    auto [addrs, err] = internetAddrList(nullptr, context::Background(), network, address);
    if (err) return {nullptr, err};
    ptr<Addr> a = forResolve(addrs, network, address);
    return {ptr<UDPAddr>(dynamic_cast<UDPAddr*>(a.get())), nullptr};
}

}  // namespace gopp::net
