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

    const segment& last_segment() const { return segments.back(); }
    const string& to_string() const noexcept { return str; }
};

std::pair<std::unique_ptr<pattern>, gopp::error> parse_pattern(std::string_view s);

enum class relationship { equivalent, more_general, more_specific, disjoint, overlaps };
std::string_view to_string(relationship r) noexcept;
relationship inverse_relationship(relationship r) noexcept;
relationship combine_relationships(relationship r1, relationship r2) noexcept;
relationship compare_methods(const pattern& p1, const pattern& p2);
relationship compare_paths(const pattern& p1, const pattern& p2);
bool conflicts_with(const pattern& p1, const pattern& p2);
string describe_conflict(const pattern& p1, const pattern& p2);
// A path that both patterns match.
string common_path(const pattern& p1, const pattern& p2);
// A path that p1 matches and p2 doesn't.
string difference_path(const pattern& p1, const pattern& p2);

// The first segment of a path starting with '/', unescaped, and the rest.
std::pair<string, std::string_view> first_segment(std::string_view path);

struct routing_node {
    const pattern* pat = nullptr;
    ptr<Handler> handler;
    std::vector<std::pair<string, std::unique_ptr<routing_node>>> children;
    std::unique_ptr<routing_node> multi_child;  // child with a multi wildcard
    std::unique_ptr<routing_node> empty_child;  // child with key ""

    void add_pattern(const pattern* p, ptr<Handler> h);
    routing_node* add_child(const string& key);
    routing_node* find_child(std::string_view key) const;

    using matches_t = std::vector<string>;
    // The leaf for host, method and path, and the wildcard values.
    const routing_node* match(std::string_view host, std::string_view method, const string& path, matches_t& m) const;
    // The methods for which host and path would match.
    void matching_methods(std::string_view host, const string& path, std::set<std::string>& methods) const;

private:
    void set(const pattern* p, ptr<Handler> h);
    void add_segments(std::span<const segment> segs, const pattern* p, ptr<Handler> h);
    const routing_node* match_method_and_path(std::string_view method, const string& path, matches_t& m) const;
    const routing_node* match_path(const string& full, std::string_view path, matches_t& m) const;
};

// Whether n matches path exactly, without a multi wildcard matching an
// empty suffix.
bool exact_match(const routing_node* n, std::string_view path);

string clean_path(const string& p);

// The handler of redirects that ServeMux makes (Go's redirectHandler).
class RedirectHandler final : public Handler {
public:
    RedirectHandler(string url, int code) : url(std::move(url)), code(code) {}
    void ServeHTTP(ResponseWriter& w, Request& r) override { Redirect(w, r, url, code); }
    string url;
    int code;
};

// ServeMux.registerErr and findHandler, for tests.
struct mux_access {
    static gopp::error register_err(ServeMux& mux, std::string_view pattern, ptr<Handler> h,
                                    std::source_location loc = std::source_location::current());
    static const routing_node* tree(const ServeMux& mux);
};

}  // namespace gopp::http::internal
