// Network interfaces and multicast, ported from Go 1.27.3's net/interface.go,
// interface_linux.go, mac.go, the multicast parts of udpsock.go and
// udpsock_posix.go, sockoptip4_linux.go and sockoptip6_posix.go, and
// syscall's netlink_linux.go (NetlinkRIB, ParseNetlinkMessage and
// ParseNetlinkRouteAttr).
#include "dial.hpp"
#include "dnsclient.hpp"

#include <gopp/os.hpp>
#include <gopp/runtime.hpp>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gopp::net {

namespace {

const error errInvalidInterfaceIndex = errors::New("invalid network interface index");
const error errInvalidInterfaceName = errors::New("invalid network interface name");
const error errNoSuchInterface = errors::New("no such network interface");

error routeError(error err) {
    auto e = make_ptr<OpError>();
    e->Op = string::fromStatic("route");
    e->Net = string::fromStatic("ip+net");
    e->Err = std::move(err);
    return e;
}

// syscall.NetlinkRIB: the dump of a routing information base (RTM_GETLINK,
// RTM_GETADDR) for family, as the kernel sends it.
std::pair<std::vector<byte>, error> netlinkRIB(int proto, int family) {
    return runtime::blocking([&]() -> std::pair<std::vector<byte>, error> {
        int s = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (s < 0) return {{}, errnoError(errno)};
        struct closer {
            int s;
            ~closer() { ::close(s); }
        } c{s};
        sockaddr_nl sa{};
        sa.nl_family = AF_NETLINK;
        if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) return {{}, errnoError(errno)};
        struct {
            nlmsghdr hdr;
            rtgenmsg gen;
        } req{};
        req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(rtgenmsg));
        req.hdr.nlmsg_type = uint16(proto);
        req.hdr.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
        req.hdr.nlmsg_seq = 1;
        req.gen.rtgen_family = uint8(family);
        if (::sendto(s, &req, req.hdr.nlmsg_len, 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
            return {{}, errnoError(errno)};
        }
        socklen_t salen = sizeof(sa);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&sa), &salen) < 0) return {{}, errnoError(errno)};
        std::vector<byte> tab;
        std::vector<byte> rb(size_t(::getpagesize()));
        for (;;) {
            ssize_t nr = ::recv(s, rb.data(), rb.size(), 0);
            if (nr < 0) {
                if (errno == EINTR) continue;
                return {{}, errnoError(errno)};
            }
            if (std::size_t(nr) < NLMSG_HDRLEN) return {{}, errnoError(EINVAL)};
            tab.insert(tab.end(), rb.begin(), rb.begin() + nr);
            // Done when this read ends with NLMSG_DONE, or has an error.
            bool done = false;
            std::size_t off = tab.size() - std::size_t(nr);
            while (off + NLMSG_HDRLEN <= tab.size()) {
                auto* h = reinterpret_cast<nlmsghdr*>(tab.data() + off);
                if (h->nlmsg_len < NLMSG_HDRLEN || off + h->nlmsg_len > tab.size()) break;
                if (h->nlmsg_seq != 1 || h->nlmsg_pid != sa.nl_pid) return {{}, errnoError(EINVAL)};
                if (h->nlmsg_type == NLMSG_DONE) done = true;
                if (h->nlmsg_type == NLMSG_ERROR) return {{}, errnoError(EINVAL)};
                off += NLMSG_ALIGN(h->nlmsg_len);
            }
            if (done) return {tab, nullptr};
        }
    });
}

// syscall.ParseNetlinkMessage and ParseNetlinkRouteAttr, as a visit of
// the messages: f(type, data, attrs) for each, attrs being (type, value).
struct rtattr_view {
    uint16 type;
    std::span<const byte> value;
};

template <typename F>
error forEachMessage(const std::vector<byte>& tab, std::size_t fixed, F f) {
    std::size_t off = 0;
    while (off + NLMSG_HDRLEN <= tab.size()) {
        auto* h = reinterpret_cast<const nlmsghdr*>(tab.data() + off);
        if (h->nlmsg_len < NLMSG_HDRLEN || off + h->nlmsg_len > tab.size()) return errnoError(EINVAL);
        if (h->nlmsg_type == NLMSG_DONE) return nullptr;
        std::span<const byte> data(tab.data() + off + NLMSG_HDRLEN, h->nlmsg_len - NLMSG_HDRLEN);
        if ((h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_NEWADDR) && data.size() >= fixed) {
            std::vector<rtattr_view> attrs;
            std::span<const byte> b = data.subspan(NLMSG_ALIGN(fixed));
            while (b.size() >= sizeof(rtattr)) {
                rtattr a;
                std::memcpy(&a, b.data(), sizeof(a));
                if (a.rta_len < sizeof(rtattr) || a.rta_len > b.size()) return errnoError(EINVAL);
                attrs.push_back({a.rta_type, b.subspan(RTA_LENGTH(0), a.rta_len - RTA_LENGTH(0))});
                std::size_t adv = RTA_ALIGN(a.rta_len);
                if (adv >= b.size()) break;
                b = b.subspan(adv);
            }
            if (bool stop = f(h->nlmsg_type, data, attrs); stop) return nullptr;
        }
        off += NLMSG_ALIGN(h->nlmsg_len);
    }
    return nullptr;
}

Flags linkFlags(uint32 rawFlags) {
    unsigned f = 0;
    if (rawFlags & IFF_UP) f |= FlagUp;
    if (rawFlags & IFF_RUNNING) f |= FlagRunning;
    if (rawFlags & IFF_BROADCAST) f |= FlagBroadcast;
    if (rawFlags & IFF_LOOPBACK) f |= FlagLoopback;
    if (rawFlags & IFF_POINTOPOINT) f |= FlagPointToPoint;
    if (rawFlags & IFF_MULTICAST) f |= FlagMulticast;
    return Flags(f);
}

// The ARP hardware types whose IFLA_ADDRESS is an IP address, not a link
// layer one.
constexpr uint16 sysARPHardwareIPv4IPv4 = 768;  // IPv4 over IPv4 tunneling
constexpr uint16 sysARPHardwareIPv6IPv6 = 769;  // IPv6 over IPv6 tunneling
constexpr uint16 sysARPHardwareIPv6IPv4 = 776;  // IPv6 over IPv4 tunneling
constexpr uint16 sysARPHardwareGREIPv4 = 778;   // any over GRE over IPv4 tunneling
constexpr uint16 sysARPHardwareGREIPv6 = 823;   // any over GRE over IPv6 tunneling

Interface newLink(const ifinfomsg& ifim, const std::vector<rtattr_view>& attrs) {
    Interface ifi;
    ifi.Index = ifim.ifi_index;
    ifi.Flags = linkFlags(ifim.ifi_flags);
    for (auto& a : attrs) {
        switch (a.type) {
        case IFLA_ADDRESS: {
            // We never return any /32 or /128 IP address prefix on any IP
            // tunnel interface as the hardware address.
            if (a.value.size() == IPv4len &&
                (ifim.ifi_type == sysARPHardwareIPv4IPv4 || ifim.ifi_type == sysARPHardwareGREIPv4 ||
                 ifim.ifi_type == sysARPHardwareIPv6IPv4)) {
                continue;
            }
            if (a.value.size() == IPv6len &&
                (ifim.ifi_type == sysARPHardwareIPv6IPv6 || ifim.ifi_type == sysARPHardwareGREIPv6)) {
                continue;
            }
            bool nonzero = false;
            for (byte b : a.value) nonzero = nonzero || b != 0;
            if (nonzero) ifi.HardwareAddr = HardwareAddr(slice<byte>::of(a.value));
            break;
        }
        case IFLA_IFNAME:
            if (!a.value.empty()) ifi.Name = string(asStringView(a.value.first(a.value.size() - 1)));
            break;
        case IFLA_MTU:
            if (a.value.size() >= 4) {
                uint32 mtu;
                std::memcpy(&mtu, a.value.data(), 4);
                ifi.MTU = int(mtu);
            }
            break;
        }
    }
    return ifi;
}

std::pair<std::vector<Interface>, error> interfaceTable(int ifindex) {
    auto [tab, err] = netlinkRIB(RTM_GETLINK, AF_UNSPEC);
    if (err) return {{}, os::NewSyscallError("netlinkrib", err)};
    std::vector<Interface> ift;
    error perr = forEachMessage(tab, sizeof(ifinfomsg),
                                [&](uint16 type, std::span<const byte> data, const std::vector<rtattr_view>& attrs) {
                                    if (type != RTM_NEWLINK) return false;
                                    ifinfomsg ifim;
                                    std::memcpy(&ifim, data.data(), sizeof(ifim));
                                    if (ifindex == 0 || ifindex == ifim.ifi_index) {
                                        ift.push_back(newLink(ifim, attrs));
                                        if (ifindex == ifim.ifi_index) return true;
                                    }
                                    return false;
                                });
    if (perr) return {{}, os::NewSyscallError("parsenetlinkmessage", perr)};
    return {ift, nullptr};
}

ptr<Addr> newAddr(const ifaddrmsg& ifam, const std::vector<rtattr_view>& attrs) {
    bool ipPointToPoint = false;
    // Seems like we need to make sure whether the IP interface stack
    // consists of IP point-to-point numbered or unnumbered addressing.
    for (auto& a : attrs) {
        if (a.type == IFA_LOCAL) {
            ipPointToPoint = true;
            break;
        }
    }
    for (auto& a : attrs) {
        if (ipPointToPoint && a.type == IFA_ADDRESS) continue;
        if (ifam.ifa_family == AF_INET && a.value.size() >= 4) {
            return make_ptr<IPNet>(IPv4(a.value[0], a.value[1], a.value[2], a.value[3]),
                                   CIDRMask(ifam.ifa_prefixlen, 8 * IPv4len));
        }
        if (ifam.ifa_family == AF_INET6 && a.value.size() >= 16) {
            return make_ptr<IPNet>(IP(slice<byte>::of(a.value.first(16))), CIDRMask(ifam.ifa_prefixlen, 8 * IPv6len));
        }
    }
    return nullptr;
}

std::pair<std::vector<ptr<Addr>>, error> interfaceAddrTable(const Interface* ifi) {
    auto [tab, err] = netlinkRIB(RTM_GETADDR, AF_UNSPEC);
    if (err) return {{}, os::NewSyscallError("netlinkrib", err)};
    std::vector<ptr<Addr>> ifat;
    error perr = forEachMessage(tab, sizeof(ifaddrmsg),
                                [&](uint16 type, std::span<const byte> data, const std::vector<rtattr_view>& attrs) {
                                    if (type != RTM_NEWADDR) return false;
                                    ifaddrmsg ifam;
                                    std::memcpy(&ifam, data.data(), sizeof(ifam));
                                    if (!ifi || ifi->Index == int(ifam.ifa_index)) {
                                        if (auto ifa = newAddr(ifam, attrs)) ifat.push_back(ifa);
                                    }
                                    return false;
                                });
    if (perr) return {{}, os::NewSyscallError("parsenetlinkmessage", perr)};
    return {ifat, nullptr};
}

std::vector<std::string> splitAtBytes(std::string_view s, std::string_view t) {
    std::vector<std::string> out;
    std::size_t last = 0;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (t.find(s[i]) != t.npos) {
            if (last < i) out.emplace_back(s.substr(last, i - last));
            last = i + 1;
        }
    }
    if (last < s.size()) out.emplace_back(s.substr(last));
    return out;
}

// xtoi2: the hex byte at s, followed by e (or the end).
bool xtoi2(std::string_view s, char e, byte* out) {
    if (s.size() > 2 && s[2] != e) return false;
    if (s.size() < 2) return false;
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[std::size_t(i)];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + d;
    }
    *out = byte(v);
    return true;
}

std::vector<std::string> readLines(const std::string& path) {
    auto [data, err] = os::ReadFile(path);
    std::vector<std::string> lines;
    if (err) return lines;
    std::string_view s = asStringView(data);
    while (!s.empty()) {
        auto nl = s.find('\n');
        lines.emplace_back(s.substr(0, nl));
        if (nl == s.npos) break;
        s.remove_prefix(nl + 1);
    }
    return lines;
}

std::vector<ptr<Addr>> parseProcNetIGMP(const std::string& path, const Interface* ifi) {
    std::vector<ptr<Addr>> ifmat;
    std::string name;
    auto lines = readLines(path);
    for (std::size_t li = 1; li < lines.size(); li++) {  // skip the first line
        const std::string& l = lines[li];
        auto f = splitAtBytes(l, " :\r\t\n");
        if (f.size() < 4) continue;
        if (l[0] != ' ' && l[0] != '\t') {  // new interface line
            name = f[1];
        } else if (f[0].size() == 8) {
            if (!ifi || name == ifi->Name.view()) {
                // The address is in the host's byte order (little-endian
                // on Linux's architectures), as Go reads it.
                byte b[4];
                for (std::size_t i = 0; i + 1 < f[0].size(); i += 2) xtoi2(std::string_view(f[0]).substr(i, 2), 0, &b[i / 2]);
                uint32 v;
                std::memcpy(&v, b, 4);
                ifmat.push_back(make_ptr<IPAddr>(IPv4(byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v))));
            }
        }
    }
    return ifmat;
}

std::vector<ptr<Addr>> parseProcNetIGMP6(const std::string& path, const Interface* ifi) {
    std::vector<ptr<Addr>> ifmat;
    for (auto& l : readLines(path)) {
        auto f = splitAtBytes(l, " \r\t\n");
        if (f.size() < 6) continue;
        if (!ifi || f[1] == ifi->Name.view()) {
            std::array<byte, 16> b{};
            for (std::size_t i = 0; i + 1 < f[2].size() && i / 2 < 16; i += 2) {
                xtoi2(std::string_view(f[2]).substr(i, 2), 0, &b[i / 2]);
            }
            ifmat.push_back(make_ptr<IPAddr>(IP(slice<byte>::of(b))));
        }
    }
    return ifmat;
}

}  // namespace

// Exposed for the tests, with Go's testdata.
std::vector<ptr<Addr>> parseProcNetIGMPForTest(const std::string& path, const Interface* ifi) {
    return parseProcNetIGMP(path, ifi);
}
std::vector<ptr<Addr>> parseProcNetIGMP6ForTest(const std::string& path, const Interface* ifi) {
    return parseProcNetIGMP6(path, ifi);
}

// ---------------------------------------------------------------------------
// mac.go

string HardwareAddr::String() const {
    if (empty()) return {};
    constexpr std::string_view hexDigit = "0123456789abcdef";
    std::string buf;
    for (std::size_t i = 0; i < size(); i++) {
        if (i > 0) buf += ':';
        buf += hexDigit[(*this)[i] >> 4];
        buf += hexDigit[(*this)[i] & 0xF];
    }
    return string(buf);
}

std::pair<HardwareAddr, error> ParseMAC(std::string_view s) {
    auto fail = [&] {
        return std::pair<HardwareAddr, error>{{}, make_ptr<AddrError>(string::fromStatic("invalid MAC address"), string(s))};
    };
    if (s.size() < 12) return fail();
    HardwareAddr hw;
    if (s[2] == ':' || s[2] == '-') {
        if ((s.size() + 1) % 3 != 0) return fail();
        std::size_t n = (s.size() + 1) / 3;
        if (n != 6 && n != 8 && n != 20) return fail();
        hw = HardwareAddr(slice<byte>::make(n));
        for (std::size_t x = 0, i = 0; i < n; i++, x += 3) {
            if (!xtoi2(s.substr(x), s[2], &hw[i])) return fail();
        }
    } else if (s[4] == '.') {
        if ((s.size() + 1) % 5 != 0) return fail();
        std::size_t n = 2 * (s.size() + 1) / 5;
        if (n != 6 && n != 8 && n != 20) return fail();
        hw = HardwareAddr(slice<byte>::make(n));
        for (std::size_t x = 0, i = 0; i < n; i += 2, x += 5) {
            if (!xtoi2(s.substr(x, 2), 0, &hw[i])) return fail();
            if (!xtoi2(s.substr(x + 2), s[4], &hw[i + 1])) return fail();
        }
    } else {
        if (s.size() % 2 != 0) return fail();
        std::size_t n = s.size() / 2;
        if (n != 6 && n != 8 && n != 20) return fail();
        hw = HardwareAddr(slice<byte>::make(n));
        for (std::size_t x = 0, i = 0; i < n; i++, x += 2) {
            if (!xtoi2(s.substr(x, 2), 0, &hw[i])) return fail();
        }
    }
    return {hw, nullptr};
}

// ---------------------------------------------------------------------------
// interface.go

string Flags::String() const {
    static const char* const flagNames[] = {"up", "broadcast", "loopback", "pointtopoint", "multicast", "running"};
    std::string s;
    for (int i = 0; i < 6; i++) {
        if (v & (1u << i)) {
            if (!s.empty()) s += "|";
            s += flagNames[i];
        }
    }
    if (s.empty()) s = "0";
    return string(s);
}

std::pair<std::vector<ptr<Addr>>, error> Interface::Addrs() const {
    auto [ifat, err] = interfaceAddrTable(this);
    if (err) return {ifat, routeError(err)};
    return {ifat, nullptr};
}

std::pair<std::vector<ptr<Addr>>, error> Interface::MulticastAddrs() const {
    auto ifmat = parseProcNetIGMP("/proc/net/igmp", this);
    auto ifmat6 = parseProcNetIGMP6("/proc/net/igmp6", this);
    ifmat.insert(ifmat.end(), ifmat6.begin(), ifmat6.end());
    return {ifmat, nullptr};
}

std::pair<std::vector<Interface>, error> Interfaces() {
    auto [ift, err] = interfaceTable(0);
    if (err) return {{}, routeError(err)};
    return {ift, nullptr};
}

std::pair<std::vector<ptr<Addr>>, error> InterfaceAddrs() {
    auto [ifat, err] = interfaceAddrTable(nullptr);
    if (err) return {ifat, routeError(err)};
    return {ifat, nullptr};
}

std::pair<ptr<Interface>, error> InterfaceByIndex(int index) {
    if (index <= 0) return {nullptr, routeError(errInvalidInterfaceIndex)};
    auto [ift, err] = interfaceTable(index);
    if (err) return {nullptr, routeError(err)};
    for (auto& ifi : ift) {
        if (ifi.Index == index) return {make_ptr<Interface>(ifi), nullptr};
    }
    return {nullptr, routeError(errNoSuchInterface)};
}

std::pair<ptr<Interface>, error> InterfaceByName(std::string_view name) {
    if (name.empty()) return {nullptr, routeError(errInvalidInterfaceName)};
    auto [ift, err] = interfaceTable(0);
    if (err) return {nullptr, routeError(err)};
    for (auto& ifi : ift) {
        if (ifi.Name.view() == name) return {make_ptr<Interface>(ifi), nullptr};
    }
    return {nullptr, routeError(errNoSuchInterface)};
}

// ---------------------------------------------------------------------------
// Multicast

namespace {

error wrapSetsockopt(int r) { return r < 0 ? os::NewSyscallError("setsockopt", errnoError(errno)) : nullptr; }

error setsockoptRaw(netfd& fd, int level, int name, const void* v, socklen_t len) {
    error err;
    fd.rawControl([&](uintptr s) { err = wrapSetsockopt(::setsockopt(int(s), level, name, v, len)); });
    return err;
}

error listenIPv4MulticastUDP(netfd& fd, const Interface* ifi, const IP& ip) {
    if (ifi) {
        ip_mreqn mreq{};
        mreq.imr_ifindex = ifi->Index;
        if (error err = setsockoptRaw(fd, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq))) return err;
    }
    int off = 0;
    if (error err = setsockoptRaw(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &off, sizeof(off))) return err;
    ip_mreqn mreq{};
    std::memcpy(&mreq.imr_multiaddr, ip.data(), 4);
    if (ifi) mreq.imr_ifindex = ifi->Index;
    return setsockoptRaw(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
}

error listenIPv6MulticastUDP(netfd& fd, const Interface* ifi, const IP& ip) {
    if (ifi) {
        int v = ifi->Index;
        if (error err = setsockoptRaw(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &v, sizeof(v))) return err;
    }
    int off = 0;
    if (error err = setsockoptRaw(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &off, sizeof(off))) return err;
    ipv6_mreq mreq{};
    std::memcpy(&mreq.ipv6mr_multiaddr, ip.To16().data(), 16);
    if (ifi) mreq.ipv6mr_interface = unsigned(ifi->Index);
    return setsockoptRaw(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));
}

}  // namespace

std::pair<ptr<UDPConn>, error> ListenMulticastUDP(std::string_view network, const ptr<Interface>& ifi,
                                                  const ptr<UDPAddr>& gaddr) {
    string net(network);
    if (network != "udp" && network != "udp4" && network != "udp6") {
        return {nullptr, op_error("listen", net, nullptr, gaddr, make_ptr<UnknownNetworkError>(net))};
    }
    if (!gaddr || gaddr->IP.empty()) return {nullptr, op_error("listen", net, nullptr, gaddr, errMissingAddress)};
    sysListener sl{.lc = {}, .network = net, .address = gaddr->String()};
    auto ctx = context::Background();
    auto [fd, err] = internetSocket(ctx, network, gaddr.get(), nullptr, SOCK_DGRAM, 0, "listen", sl.ctrlCtxFn());
    if (err) return {nullptr, op_error("listen", net, nullptr, gaddr, err)};
    auto c = make_ptr<UDPConn>(fd);
    if (IP ip4 = gaddr->IP.To4(); !ip4.empty()) err = listenIPv4MulticastUDP(*fd, ifi.get(), ip4);
    else err = listenIPv6MulticastUDP(*fd, ifi.get(), gaddr->IP);
    if (err) {
        c->Close();
        return {nullptr, op_error("listen", net, nullptr, gaddr, err)};
    }
    return {c, nullptr};
}

}  // namespace gopp::net
