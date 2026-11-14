// ServeMux internals, after Go's net/http/pattern.Go and routing_tree.Go.
// Declared here so that tests can reach them, as Go's in-package tests do.
#pragma once

#include <gopp/net/http.hpp>

#include <memory>
#include <set>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gopp::http::internal {

struct segment {
    string s;  // literal, wildcard name, or "/" for "{$}"
    bool wild = false;
    bool multi = false;  // "{name...}" or a trailing slash
    friend bool operator==(const segment&, const segment&) = default;
};

struct pattern {
    string str;
    string method;
    string host;
    std::vector<segment> segments;
    string loc;  // where it was registered, for conflict messages

    const segment& lastSegment() const { return segments.back(); }
    const string& to_string() const noexcept { return str; }
};

std::pair<std::unique_ptr<pattern>, gopp::error> parsePattern(std::string_view s);

enum class relationship { equivalent, moreGeneral, moreSpecific, disjoint, overlaps };
std::string_view to_string(relationship r) noexcept;
relationship inverseRelationship(relationship r) noexcept;
relationship combineRelationships(relationship r1, relationship r2) noexcept;
relationship compareMethods(const pattern& p1, const pattern& p2);
relationship comparePaths(const pattern& p1, const pattern& p2);
bool conflictsWith(const pattern& p1, const pattern& p2);
string describeConflict(const pattern& p1, const pattern& p2);
// A path that both patterns match.
string commonPath(const pattern& p1, const pattern& p2);
// A path that p1 matches and p2 doesn't.
string differencePath(const pattern& p1, const pattern& p2);

// The first segment of a path starting with '/', unescaped, and the rest.
std::pair<string, std::string_view> firstSegment(std::string_view path);

struct routingNode {
    const pattern* pat = nullptr;
    ptr<Handler> handler;
    std::vector<std::pair<string, std::unique_ptr<routingNode>>> children;
    std::unique_ptr<routingNode> multiChild;  // child with a multi wildcard
    std::unique_ptr<routingNode> emptyChild;  // child with key ""

    void addPattern(const pattern* p, ptr<Handler> h);
    routingNode* addChild(const string& key);
    routingNode* findChild(std::string_view key) const;

    using matchesT = std::vector<string>;
    // The leaf for host, method and path, and the wildcard values.
    const routingNode* match(std::string_view host, std::string_view method, const string& path, matchesT& m) const;
    // The methods for which host and path would match.
    void matchingMethods(std::string_view host, const string& path, std::set<std::string>& methods) const;

private:
    void set(const pattern* p, ptr<Handler> h);
    void addSegments(std::span<const segment> segs, const pattern* p, ptr<Handler> h);
    const routingNode* matchMethodAndPath(std::string_view method, const string& path, matchesT& m) const;
    const routingNode* matchPath(const string& full, std::string_view path, matchesT& m) const;
};

// Whether n matches path exactly, without a multi wildcard matching an
// empty suffix.
bool exactMatch(const routingNode* n, std::string_view path);

string cleanPath(const string& p);

// The handler of redirects that ServeMux makes (Go's redirectHandler).
class RedirectHandler final : public Handler {
public:
    RedirectHandler(string url, int code) : url(std::move(url)), code(code) {}
    void ServeHTTP(ResponseWriter& w, Request& r) override { Redirect(w, r, url, code); }
    string url;
    int code;
};

// ServeMux.registerErr and findHandler, for tests.
struct muxAccess {
    static gopp::error registerErr(ServeMux& mux, std::string_view pattern, ptr<Handler> h,
                                    std::source_location loc = std::source_location::current());
    static const routingNode* tree(const ServeMux& mux);
};

}  // namespace gopp::http::internal
