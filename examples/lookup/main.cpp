// lookup: looks names up in DNS, all at once. Every record type of every
// name is a query of its own fiber, on go++'s port of Go's resolver, and
// all of them share one deadline through a context.
//
//     $ lookup go.dev example.com
//     $ lookup -types mx,txt -timeout 500ms gmail.com
//     $ lookup -types ptr 8.8.8.8
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <functional>
#include <map>
#include <vector>

using namespace gopp;

// The answer to one query.
struct answer {
    std::size_t name;  // the index of the name
    string type;
    std::vector<string> records;
    error err;
};

using query_fn = std::function<std::pair<std::vector<string>, error>(
    net::Resolver&, const ptr<context::Context>&,
    std::string_view)>;

// The record types, in the order they're printed.
const std::vector<std::pair<std::string_view, query_fn>>& queries() {
    static const std::vector<std::pair<std::string_view, query_fn>> q = {
        {"ip", [](net::Resolver& r, const ptr<context::Context>& ctx, std::string_view name) {
             auto [addrs, err] = r.LookupNetIP(ctx, "ip", name);
             std::vector<string> out;
             // IPv4 addresses from /etc/hosts come as IPv4-mapped IPv6
             // ones (::ffff:127.0.0.1), as in Go: Unmap makes them IPv4.
             for (const auto& a : addrs) out.push_back(a.Unmap().String());
             return std::pair(out, err);
         }},
        {"cname", [](net::Resolver& r, const ptr<context::Context>& ctx, std::string_view name) {
             auto [cname, err] = r.LookupCNAME(ctx, name);
             return std::pair(err ? std::vector<string>{} : std::vector<string>{cname}, err);
         }},
        {"mx", [](net::Resolver& r, const ptr<context::Context>& ctx, std::string_view name) {
             auto [mxs, err] = r.LookupMX(ctx, name);
             std::vector<string> out;
             for (const auto& mx : mxs) out.push_back(fmt::Sprint("{} {}", mx->Pref, mx->Host));
             return std::pair(out, err);
         }},
        {"ns", [](net::Resolver& r, const ptr<context::Context>& ctx, std::string_view name) {
             auto [nss, err] = r.LookupNS(ctx, name);
             std::vector<string> out;
             for (const auto& ns : nss) out.push_back(ns->Host);
             return std::pair(out, err);
         }},
        {"txt", [](net::Resolver& r, const ptr<context::Context>& ctx, std::string_view name) {
             auto [txts, err] = r.LookupTXT(ctx, name);
             std::vector<string> out;
             for (const auto& t : txts) out.push_back(strconv::Quote(t));
             return std::pair(out, err);
         }},
        {"ptr", [](net::Resolver& r, const ptr<context::Context>& ctx, std::string_view addr) {
             return r.LookupAddr(ctx, addr);
         }},
    };
    return q;
}

int main(int argc, char** argv) {
    string types = "ip,cname,mx,ns,txt";
    time::Duration timeout = 3 * time::Second;
    flag::StringVar(&types, "types", types, "the record types to look up: ip, cname, mx, ns, txt, ptr");
    flag::DurationVar(&timeout, "timeout", timeout, "the time limit for all the lookups");
    flag::Parse(argc, argv);
    const auto& names = flag::Args();
    if (names.empty()) {
        fmt::Println(os::Stderr, "usage: lookup [-types ip,mx,...] [-timeout d] name...");
        return 2;
    }

    std::vector<const std::pair<std::string_view, query_fn>*> wanted;
    for (const string& t : strings::Split(types, ",")) {
        const std::pair<std::string_view, query_fn>* found = nullptr;
        for (const auto& q : queries()) {
            if (q.first == strings::TrimSpace(t)) found = &q;
        }
        if (!found) {
            fmt::Println(os::Stderr, "lookup: unknown record type {}", strconv::Quote(t));
            return 2;
        }
        wanted.push_back(found);
    }

    // One fiber per name and type; the answers come back on a channel.
    auto [ctx, cancel] = context::WithTimeout(context::Background(), timeout);
    auto resolver = make_ptr<net::Resolver>();
    auto answers = makeChan<answer>(names.size() * wanted.size());
    time::Time start = time::Now();
    for (std::size_t i = 0; i < names.size(); i++) {
        for (const auto* q : wanted) {
            go([=] {
                auto [records, err] = q->second(*resolver, ctx, names[i]);
                answers.send(answer{i, string(q->first), std::move(records), err});
            });
        }
    }

    // Collect them all, then print them by name and type.
    std::map<std::pair<std::size_t, std::size_t>, answer> by_query;
    for (std::size_t n = 0; n < names.size() * wanted.size(); n++) {
        answer a = answers.recv();
        std::size_t t = 0;
        while (wanted[t]->first != a.type) t++;
        by_query.emplace(std::pair(a.name, t), std::move(a));
    }
    cancel();

    int failed = 0;
    for (std::size_t i = 0; i < names.size(); i++) {
        fmt::Println("{}", names[i]);
        for (std::size_t t = 0; t < wanted.size(); t++) {
            const answer& a = by_query.at({i, t});
            if (a.err) {
                failed++;
                // A name without records of the type isn't much of an error.
                auto* dns = errors::As<net::DNSError>(a.err);
                fmt::Println("  {:<6} {}", a.type, dns && dns->IsNotFound ? string::fromStatic("-") : a.err.Error());
                continue;
            }
            for (std::size_t r = 0; r < a.records.size(); r++) {
                fmt::Println("  {:<6} {}", r == 0 ? a.type : string(), a.records[r]);
            }
        }
    }
    fmt::Println("{} lookups in {}", names.size() * wanted.size(), time::Since(start));
    return failed == static_cast<int>(names.size() * wanted.size()) ? 1 : 0;
}
