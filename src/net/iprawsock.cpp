// IP protocol numbers, after Go's net/lookup.go (protocols,
// lookupProtocolMap) and lookup_unix.go (readProtocols, lookupProtocol).
#include "dial.hpp"

#include <gopp/runtime.hpp>

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace gopp::net {

namespace {

constexpr std::size_t maxProtoLength = sizeof("RSVP-E2E-IGNORE") - 1 + 10;

// The minimal mappings, completed from /etc/protocols on first use.
const std::map<std::string, int, std::less<>>& protocols() {
    static std::map<std::string, int, std::less<>> m = [] {
        std::map<std::string, int, std::less<>> p = {
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
    }();
    return m;
}

}  // namespace

std::pair<int, error> lookupProtocol(const ptr<context::Context>&, std::string_view name) {
    std::string lower(name.substr(0, std::min(name.size(), maxProtoLength)));
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto& m = protocols();
    auto it = m.find(lower);
    if (it == m.end() || lower.size() != name.size()) {
        return {0, make_ptr<AddrError>(string::from_static("unknown IP protocol specified"), string(name))};
    }
    return {it->second, nullptr};
}

}  // namespace gopp::net
