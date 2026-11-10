// Tests ported from Go 1.27.3: net/http/pattern_test.Go,
// routing_tree_test.Go, server_test.Go, and the ServeMux and path value
// tests of serve_test.Go and request_test.Go.
//
// Copyright 2023 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in Go's LICENSE file.
#include "net/http/internal.hpp"
#include "net/http/routing.hpp"

#include <gopp/fmt.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <regex>
#include <set>

using namespace gopp;
using namespace gopp::http::internal;
using gopp::http::Request;

namespace {

std::unique_ptr<pattern> must_parse_pattern(std::string_view s) {
    auto [p, err] = parse_pattern(s);
    if (err) ADD_FAILURE() << s << ": " << err;
    return std::move(p);
}

segment lit(std::string_view name) { return segment{string(name), false, false}; }
segment wild(std::string_view name) { return segment{string(name), true, false}; }
segment multi(std::string_view name) { return segment{string(name), true, true}; }

std::string str(relationship r) { return std::string(to_string(r)); }

}  // namespace

// ---------------------------------------------------------------------------
// pattern_test.Go

TEST(GoNetHttp, ParsePattern) {
    struct want_t {
        std::string method, host;
        std::vector<segment> segments;
    };
    struct {
        std::string in;
        want_t want;
    } tests[] = {
        {"/", {"", "", {multi("")}}},
        {"/a", {"", "", {lit("a")}}},
        {"/a/", {"", "", {lit("a"), multi("")}}},
        {"/path/to/something", {"", "", {lit("path"), lit("to"), lit("something")}}},
        {"/{w1}/lit/{w2}", {"", "", {wild("w1"), lit("lit"), wild("w2")}}},
        {"/{w1}/lit/{w2}/", {"", "", {wild("w1"), lit("lit"), wild("w2"), multi("")}}},
        {"example.com/", {"", "example.com", {multi("")}}},
        {"GET /", {"GET", "", {multi("")}}},
        {"POST example.com/foo/{w}", {"POST", "example.com", {lit("foo"), wild("w")}}},
        {"/{$}", {"", "", {lit("/")}}},
        {"DELETE example.com/a/{foo12}/{$}", {"DELETE", "example.com", {lit("a"), wild("foo12"), lit("/")}}},
        {"/foo/{$}", {"", "", {lit("foo"), lit("/")}}},
        {"/{a}/foo/{rest...}", {"", "", {wild("a"), lit("foo"), multi("rest")}}},
        {"//", {"", "", {lit(""), multi("")}}},
        {"/foo///./../bar", {"", "", {lit("foo"), lit(""), lit(""), lit("."), lit(".."), lit("bar")}}},
        {"a.com/foo//", {"", "a.com", {lit("foo"), lit(""), multi("")}}},
        {"/%61%62/%7b/%", {"", "", {lit("ab"), lit("{"), lit("%")}}},
        // Allow multiple spaces matching regexp '[ \t]+' between method and path.
        {"GET\t  /", {"GET", "", {multi("")}}},
        {"POST \t  example.com/foo/{w}", {"POST", "example.com", {lit("foo"), wild("w")}}},
        {"DELETE    \texample.com/a/{foo12}/{$}", {"DELETE", "example.com", {lit("a"), wild("foo12"), lit("/")}}},
    };
    for (auto& test : tests) {
        auto got = must_parse_pattern(test.in);
        ASSERT_TRUE(got) << test.in;
        EXPECT_EQ(got->method, test.want.method) << test.in;
        EXPECT_EQ(got->host, test.want.host) << test.in;
        EXPECT_TRUE(got->segments == test.want.segments) << test.in;
    }
}

TEST(GoNetHttp, ParsePatternError) {
    struct {
        std::string in, contains;
    } tests[] = {
        {"", "empty pattern"},
        {"A=B /", "at offset 0: invalid method"},
        {" ", "at offset 1: host/path missing /"},
        {"/{w}x", "at offset 1: bad wildcard segment"},
        {"/x{w}", "at offset 1: bad wildcard segment"},
        {"/{wx", "at offset 1: bad wildcard segment"},
        {"/a/{/}/c", "at offset 3: bad wildcard segment"},
        {"/a/{%61}/c", "at offset 3: bad wildcard name"},  // wildcard names aren't unescaped
        {"/{a$}", "at offset 1: bad wildcard name"},
        {"/{}", "at offset 1: empty wildcard"},
        {"POST a.com/x/{}/y", "at offset 13: empty wildcard"},
        {"/{...}", "at offset 1: empty wildcard"},
        {"/{$...}", "at offset 1: bad wildcard"},
        {"/{$}/", "at offset 1: {$} not at end"},
        {"/{$}/x", "at offset 1: {$} not at end"},
        {"/abc/{$}/x", "at offset 5: {$} not at end"},
        {"/{a...}/", "at offset 1: {...} wildcard not at end"},
        {"/{a...}/x", "at offset 1: {...} wildcard not at end"},
        {"{a}/b", "at offset 0: host contains '{' (missing initial '/'?)"},
        {"/a/{x}/b/{x...}", "at offset 9: duplicate wildcard name"},
        {"GET //", "at offset 4: non-CONNECT pattern with unclean path"},
    };
    for (auto& test : tests) {
        auto [p, err] = parse_pattern(test.in);
        ASSERT_TRUE(err) << test.in;
        EXPECT_NE(std::string(err.Error().view()).find(test.contains), std::string::npos)
            << test.in << ": got " << err << ", want error containing " << test.contains;
    }
}

TEST(GoNetHttp, CompareMethods) {
    struct {
        std::string p1, p2;
        relationship want;
    } tests[] = {
        {"/", "/", relationship::equivalent},
        {"GET /", "GET /", relationship::equivalent},
        {"HEAD /", "HEAD /", relationship::equivalent},
        {"POST /", "POST /", relationship::equivalent},
        {"GET /", "POST /", relationship::disjoint},
        {"GET /", "/", relationship::more_specific},
        {"HEAD /", "/", relationship::more_specific},
        {"GET /", "HEAD /", relationship::more_general},
    };
    for (auto& test : tests) {
        auto pat1 = must_parse_pattern(test.p1);
        auto pat2 = must_parse_pattern(test.p2);
        EXPECT_EQ(str(compare_methods(*pat1, *pat2)), str(test.want)) << test.p1 << " vs " << test.p2;
        EXPECT_EQ(str(compare_methods(*pat2, *pat1)), str(inverse_relationship(test.want))) << test.p2 << " vs " << test.p1;
    }
}

TEST(GoNetHttp, ComparePaths) {
    using enum relationship;
    struct {
        std::string p1, p2;
        relationship want;
    } tests[] = {
        // A non-final pattern segment can have one of two values: literal or
        // single wildcard. A final pattern segment can have one of 5: empty
        // (trailing slash), literal, dollar, single wildcard, or multi
        // wildcard. Trailing slash and multi wildcard are the same.

        // A literal should be more specific than anything it overlaps, except itself.
        {"/a", "/a", equivalent},
        {"/a", "/b", disjoint},
        {"/a", "/", more_specific},
        {"/a", "/{$}", disjoint},
        {"/a", "/{x}", more_specific},
        {"/a", "/{x...}", more_specific},

        // Adding a segment doesn't change that.
        {"/b/a", "/b/a", equivalent},
        {"/b/a", "/b/b", disjoint},
        {"/b/a", "/b/", more_specific},
        {"/b/a", "/b/{$}", disjoint},
        {"/b/a", "/b/{x}", more_specific},
        {"/b/a", "/b/{x...}", more_specific},
        {"/{z}/a", "/{z}/a", equivalent},
        {"/{z}/a", "/{z}/b", disjoint},
        {"/{z}/a", "/{z}/", more_specific},
        {"/{z}/a", "/{z}/{$}", disjoint},
        {"/{z}/a", "/{z}/{x}", more_specific},
        {"/{z}/a", "/{z}/{x...}", more_specific},

        // Single wildcard on left.
        {"/{z}", "/a", more_general},
        {"/{z}", "/a/b", disjoint},
        {"/{z}", "/{$}", disjoint},
        {"/{z}", "/{x}", equivalent},
        {"/{z}", "/", more_specific},
        {"/{z}", "/{x...}", more_specific},
        {"/b/{z}", "/b/a", more_general},
        {"/b/{z}", "/b/a/b", disjoint},
        {"/b/{z}", "/b/{$}", disjoint},
        {"/b/{z}", "/b/{x}", equivalent},
        {"/b/{z}", "/b/", more_specific},
        {"/b/{z}", "/b/{x...}", more_specific},

        // Trailing slash on left.
        {"/", "/a", more_general},
        {"/", "/a/b", more_general},
        {"/", "/{$}", more_general},
        {"/", "/{x}", more_general},
        {"/", "/", equivalent},
        {"/", "/{x...}", equivalent},

        {"/b/", "/b/a", more_general},
        {"/b/", "/b/a/b", more_general},
        {"/b/", "/b/{$}", more_general},
        {"/b/", "/b/{x}", more_general},
        {"/b/", "/b/", equivalent},
        {"/b/", "/b/{x...}", equivalent},

        {"/{z}/", "/{z}/a", more_general},
        {"/{z}/", "/{z}/a/b", more_general},
        {"/{z}/", "/{z}/{$}", more_general},
        {"/{z}/", "/{z}/{x}", more_general},
        {"/{z}/", "/{z}/", equivalent},
        {"/{z}/", "/a/", more_general},
        {"/{z}/", "/{z}/{x...}", equivalent},
        {"/{z}/", "/a/{x...}", more_general},
        {"/a/{z}/", "/{z}/a/", overlaps},
        {"/a/{z}/b/", "/{x}/c/{y...}", overlaps},

        // Multi wildcard on left.
        {"/{m...}", "/a", more_general},
        {"/{m...}", "/a/b", more_general},
        {"/{m...}", "/{$}", more_general},
        {"/{m...}", "/{x}", more_general},
        {"/{m...}", "/", equivalent},
        {"/{m...}", "/{x...}", equivalent},

        {"/b/{m...}", "/b/a", more_general},
        {"/b/{m...}", "/b/a/b", more_general},
        {"/b/{m...}", "/b/{$}", more_general},
        {"/b/{m...}", "/b/{x}", more_general},
        {"/b/{m...}", "/b/", equivalent},
        {"/b/{m...}", "/b/{x...}", equivalent},
        {"/b/{m...}", "/a/{x...}", disjoint},

        {"/{z}/{m...}", "/{z}/a", more_general},
        {"/{z}/{m...}", "/{z}/a/b", more_general},
        {"/{z}/{m...}", "/{z}/{$}", more_general},
        {"/{z}/{m...}", "/{z}/{x}", more_general},
        {"/{z}/{m...}", "/{w}/", equivalent},
        {"/{z}/{m...}", "/a/", more_general},
        {"/{z}/{m...}", "/{z}/{x...}", equivalent},
        {"/{z}/{m...}", "/a/{x...}", more_general},
        {"/a/{m...}", "/a/b/{y...}", more_general},
        {"/a/{m...}", "/a/{x}/{y...}", more_general},
        {"/a/{z}/{m...}", "/a/b/{y...}", more_general},
        {"/a/{z}/{m...}", "/{z}/a/", overlaps},
        {"/a/{z}/{m...}", "/{z}/b/{y...}", overlaps},
        {"/a/{z}/b/{m...}", "/{x}/c/{y...}", overlaps},
        {"/a/{z}/a/{m...}", "/{x}/b", disjoint},

        // Dollar on left.
        {"/{$}", "/a", disjoint},
        {"/{$}", "/a/b", disjoint},
        {"/{$}", "/{$}", equivalent},
        {"/{$}", "/{x}", disjoint},
        {"/{$}", "/", more_specific},
        {"/{$}", "/{x...}", more_specific},

        {"/b/{$}", "/b", disjoint},
        {"/b/{$}", "/b/a", disjoint},
        {"/b/{$}", "/b/a/b", disjoint},
        {"/b/{$}", "/b/{$}", equivalent},
        {"/b/{$}", "/b/{x}", disjoint},
        {"/b/{$}", "/b/", more_specific},
        {"/b/{$}", "/b/{x...}", more_specific},
        {"/b/{$}", "/b/c/{x...}", disjoint},
        {"/b/{x}/a/{$}", "/{x}/c/{y...}", overlaps},
        {"/{x}/b/{$}", "/a/{x}/{y}", disjoint},
        {"/{x}/b/{$}", "/a/{x}/c", disjoint},

        {"/{z}/{$}", "/{z}/a", disjoint},
        {"/{z}/{$}", "/{z}/a/b", disjoint},
        {"/{z}/{$}", "/{z}/{$}", equivalent},
        {"/{z}/{$}", "/{z}/{x}", disjoint},
        {"/{z}/{$}", "/{z}/", more_specific},
        {"/{z}/{$}", "/a/", overlaps},
        {"/{z}/{$}", "/a/{x...}", overlaps},
        {"/{z}/{$}", "/{z}/{x...}", more_specific},
        {"/a/{z}/{$}", "/{z}/a/", overlaps},
    };
    for (auto& test : tests) {
        auto pat1 = must_parse_pattern(test.p1);
        auto pat2 = must_parse_pattern(test.p2);
        EXPECT_EQ(str(compare_paths(*pat1, *pat1)), "equivalent") << test.p1 << " does not match itself";
        EXPECT_EQ(str(compare_paths(*pat2, *pat2)), "equivalent") << test.p2 << " does not match itself";
        EXPECT_EQ(str(compare_paths(*pat1, *pat2)), str(test.want)) << test.p1 << " vs " << test.p2;
        EXPECT_EQ(str(compare_paths(*pat2, *pat1)), str(inverse_relationship(test.want))) << test.p2 << " vs " << test.p1;
    }
}

TEST(GoNetHttp, ConflictsWith) {
    struct {
        std::string p1, p2;
        bool want;
    } tests[] = {
        {"/a", "/a", true},
        {"/a", "/ab", false},
        {"/a/b/cd", "/a/b/cd", true},
        {"/a/b/cd", "/a/b/c", false},
        {"/a/b/c", "/a/c/c", false},
        {"/{x}", "/{y}", true},
        {"/{x}", "/a", false},  // more specific
        {"/{x}/{y}", "/{x}/a", false},
        {"/{x}/{y}", "/{x}/a/b", false},
        {"/{x}", "/a/{y}", false},
        {"/{x}/{y}", "/{x}/a/", false},
        {"/{x}", "/a/{y...}", false},            // more specific
        {"/{x}/a/{y}", "/{x}/a/{y...}", false},  // more specific
        {"/{x}/{y}", "/{x}/a/{$}", false},       // more specific
        {"/{x}/{y}/{$}", "/{x}/a/{$}", false},
        {"/a/{x}", "/{x}/b", true},
        {"/", "GET /", false},
        {"/", "GET /foo", false},
        {"GET /", "GET /foo", false},
        {"GET /", "/foo", true},
        {"GET /foo", "HEAD /", true},
    };
    for (auto& test : tests) {
        auto pat1 = must_parse_pattern(test.p1);
        auto pat2 = must_parse_pattern(test.p2);
        EXPECT_EQ(conflicts_with(*pat1, *pat2), test.want) << test.p1 << ".ConflictsWith(" << test.p2 << ")";
        // conflictsWith should be commutative.
        EXPECT_EQ(conflicts_with(*pat2, *pat1), test.want) << test.p2 << ".ConflictsWith(" << test.p1 << ")";
    }
}

TEST(GoNetHttp, RegisterConflict) {
    http::ServeMux mux;
    ASSERT_FALSE(mux_access::register_err(mux, "/a/{x}/", http::NotFoundHandler()));
    auto err = mux_access::register_err(mux, "/a/{y}/{z...}", http::NotFoundHandler());
    std::string got = err ? std::string(err.Error().view()) : "<nil>";
    EXPECT_NE(got.find("matches the same requests as"), std::string::npos) << got;
}

TEST(GoNetHttp, DescribeConflict) {
    struct {
        std::string p1, p2, want;
    } tests[] = {
        {"/a/{x}", "/a/{y}", "the same requests"},
        {"/", "/{m...}", "the same requests"},
        {"/a/{x}", "/{y}/b", "both match some paths"},
        {"/a", "GET /{x}", "matches more methods than GET /{x}, but has a more specific path pattern"},
        {"GET /a", "HEAD /", "matches more methods than HEAD /, but has a more specific path pattern"},
        {"POST /", "/a", "matches fewer methods than /a, but has a more general path pattern"},
    };
    for (auto& test : tests) {
        std::string got(describe_conflict(*must_parse_pattern(test.p1), *must_parse_pattern(test.p2)).view());
        EXPECT_NE(got.find(test.want), std::string::npos) << test.p1 << " vs. " << test.p2 << ":\n" << got;
    }
}

TEST(GoNetHttp, CommonPath) {
    struct {
        std::string p1, p2, want;
    } tests[] = {
        {"/a/{x}", "/{x}/a", "/a/a"},
        {"/a/{z}/", "/{z}/a/", "/a/a/"},
        {"/a/{z}/{m...}", "/{z}/a/", "/a/a/"},
        {"/{z}/{$}", "/a/", "/a/"},
        {"/{z}/{$}", "/a/{x...}", "/a/"},
        {"/a/{z}/{$}", "/{z}/a/", "/a/a/"},
        {"/a/{x}/b/{y...}", "/{x}/c/{y...}", "/a/c/b/"},
        {"/a/{x}/b/", "/{x}/c/{y...}", "/a/c/b/"},
        {"/a/{x}/b/{$}", "/{x}/c/{y...}", "/a/c/b/"},
        {"/a/{z}/{x...}", "/{z}/b/{y...}", "/a/b/"},
    };
    for (auto& test : tests) {
        auto pat1 = must_parse_pattern(test.p1);
        auto pat2 = must_parse_pattern(test.p2);
        ASSERT_EQ(str(compare_paths(*pat1, *pat2)), "overlaps") << test.p1 << " does not overlap " << test.p2;
        EXPECT_EQ(common_path(*pat1, *pat2), test.want) << test.p1 << " vs. " << test.p2;
    }
}

TEST(GoNetHttp, DifferencePath) {
    struct {
        std::string p1, p2, want;
    } tests[] = {
        {"/a/{x}", "/{x}/a", "/a/x"},
        {"/{x}/a", "/a/{x}", "/x/a"},
        {"/a/{z}/", "/{z}/a/", "/a/z/"},
        {"/{z}/a/", "/a/{z}/", "/z/a/"},
        {"/{a}/a/", "/a/{z}/", "/ax/a/"},
        {"/a/{z}/{x...}", "/{z}/b/{y...}", "/a/z/"},
        {"/{z}/b/{y...}", "/a/{z}/{x...}", "/z/b/"},
        {"/a/b/", "/a/b/c", "/a/b/"},
        {"/a/b/{x...}", "/a/b/c", "/a/b/"},
        {"/a/b/{x...}", "/a/b/c/d", "/a/b/"},
        {"/a/b/{x...}", "/a/b/c/d/", "/a/b/"},
        {"/a/{z}/{m...}", "/{z}/a/", "/a/z/"},
        {"/{z}/a/", "/a/{z}/{m...}", "/z/a/"},
        {"/{z}/{$}", "/a/", "/z/"},
        {"/a/", "/{z}/{$}", "/a/x"},
        {"/{z}/{$}", "/a/{x...}", "/z/"},
        {"/a/{foo...}", "/{z}/{$}", "/a/foo"},
        {"/a/{z}/{$}", "/{z}/a/", "/a/z/"},
        {"/{z}/a/", "/a/{z}/{$}", "/z/a/x"},
        {"/a/{x}/b/{y...}", "/{x}/c/{y...}", "/a/x/b/"},
        {"/{x}/c/{y...}", "/a/{x}/b/{y...}", "/x/c/"},
        {"/a/{c}/b/", "/{x}/c/{y...}", "/a/cx/b/"},
        {"/{x}/c/{y...}", "/a/{c}/b/", "/x/c/"},
        {"/a/{x}/b/{$}", "/{x}/c/{y...}", "/a/x/b/"},
        {"/{x}/c/{y...}", "/a/{x}/b/{$}", "/x/c/"},
    };
    for (auto& test : tests) {
        auto pat1 = must_parse_pattern(test.p1);
        auto pat2 = must_parse_pattern(test.p2);
        relationship rel = compare_paths(*pat1, *pat2);
        ASSERT_TRUE(rel == relationship::overlaps || rel == relationship::more_general)
            << test.p1 << " vs. " << test.p2 << " are " << str(rel) << ", need overlaps or moreGeneral";
        EXPECT_EQ(difference_path(*pat1, *pat2), test.want) << test.p1 << " vs. " << test.p2;
    }
}

// ---------------------------------------------------------------------------
// routing_tree_test.Go

namespace {

// Patterns parsed for a tree must outlive it.
struct test_tree {
    std::vector<std::unique_ptr<pattern>> pats;
    routing_node root;
};

std::unique_ptr<test_tree> build_tree(std::initializer_list<std::string_view> pats) {
    auto t = std::make_unique<test_tree>();
    for (auto p : pats) {
        auto pat = must_parse_pattern(p);
        t->root.add_pattern(pat.get(), nullptr);
        t->pats.push_back(std::move(pat));
    }
    return t;
}

// routingNode.Print
void print(const routing_node& n, std::string& w, int level) {
    std::string indent(static_cast<std::size_t>(level) * 4, ' ');
    if (n.pat) w += indent + std::string(strconv::Quote(n.pat->str).view()) + "\n";
    if (n.empty_child) {
        w += indent + "\"\":\n";
        print(*n.empty_child, w, level + 1);
    }
    std::vector<std::string> keys;
    for (auto& [k, c] : n.children) keys.emplace_back(k.view());
    std::sort(keys.begin(), keys.end());
    for (auto& k : keys) {
        w += indent + std::string(strconv::Quote(k).view()) + ":\n";
        print(*n.find_child(k), w, level + 1);
    }
    if (n.multi_child) {
        w += indent + "MULTI:\n";
        print(*n.multi_child, w, level + 1);
    }
}

}  // namespace

TEST(GoNetHttp, RoutingFirstSegment) {
    struct {
        std::string in;
        std::vector<std::string> want;
    } tests[] = {
        {"/a/b/c", {"a", "b", "c"}},
        {"/a/b/", {"a", "b", "/"}},
        {"/", {"/"}},
        {"/a/%62/c", {"a", "b", "c"}},
        {"/a%2Fb%2fc", {"a/b/c"}},
    };
    for (auto& test : tests) {
        std::vector<std::string> got;
        std::string_view rest = test.in;
        while (!rest.empty()) {
            auto [seg, r] = first_segment(rest);
            got.emplace_back(seg.view());
            rest = r;
        }
        EXPECT_EQ(got, test.want) << test.in;
    }
}

TEST(GoNetHttp, RoutingAddPattern) {
    const char* want = R"("":
    "":
        "a":
            "/a"
            "":
                "/a/{x}"
            "b":
                "/a/b"
                "":
                    "/a/b/{y}"
                "/":
                    "/a/b/{$}"
                MULTI:
                    "/a/b/{x...}"
        "g":
            "":
                "j":
                    "/g/{x}/j"
            "h":
                "i":
                    "/g/h/i"
)";
    auto t = build_tree({"/a", "/a/b", "/a/{x}", "/g/h/i", "/g/{x}/j", "/a/b/{x...}", "/a/b/{y}", "/a/b/{$}"});
    std::string got;
    print(t->root, got, 0);
    EXPECT_EQ(got, want);
}

namespace {

struct test_case {
    std::string method, host, path;
    std::string want_pat;  // "" for no match
    std::vector<std::string> want_matches;
};

void run_match_tests(const routing_node& tree, std::initializer_list<test_case> tests) {
    for (auto& test : tests) {
        routing_node::matches_t m;
        const routing_node* got_node = tree.match(test.host, test.method, string(test.path), m);
        std::string got = got_node ? std::string(got_node->pat->str.view()) : "";
        EXPECT_EQ(got, test.want_pat) << test.host << ", " << test.method << ", " << test.path;
        std::vector<std::string> got_matches;
        for (auto& s : m) got_matches.emplace_back(s.view());
        EXPECT_EQ(got_matches, test.want_matches) << test.host << ", " << test.method << ", " << test.path;
    }
}

}  // namespace

TEST(GoNetHttp, RoutingNodeMatch) {
    auto t = build_tree({"/a", "/a/b", "/a/{x}", "/g/h/i", "/g/{x}/j", "/a/b/{x...}", "/a/b/{y}", "/a/b/{$}"});
    run_match_tests(t->root, {
                                 {"GET", "", "/a", "/a", {}},
                                 {"Get", "", "/b", "", {}},
                                 {"Get", "", "/a/b", "/a/b", {}},
                                 {"Get", "", "/a/c", "/a/{x}", {"c"}},
                                 {"Get", "", "/a/b/", "/a/b/{$}", {}},
                                 {"Get", "", "/a/b/c", "/a/b/{y}", {"c"}},
                                 {"Get", "", "/a/b/c/d", "/a/b/{x...}", {"c/d"}},
                                 {"Get", "", "/g/h/i", "/g/h/i", {}},
                                 {"Get", "", "/g/h/j", "/g/{x}/j", {"h"}},
                             });

    auto tree = build_tree({"/item/", "POST /item/{user}", "GET /item/{user}", "/item/{user}", "/item/{user}/{id}",
                            "/item/{user}/new", "/item/{$}", "POST alt.com/item/{user}", "GET /headwins",
                            "HEAD /headwins", "/path/{p...}"});
    run_match_tests(tree->root, {
                                    {"GET", "", "/item/jba", "GET /item/{user}", {"jba"}},
                                    {"POST", "", "/item/jba", "POST /item/{user}", {"jba"}},
                                    {"HEAD", "", "/item/jba", "GET /item/{user}", {"jba"}},
                                    {"get", "", "/item/jba", "/item/{user}", {"jba"}},  // method matches are case-sensitive
                                    {"POST", "", "/item/jba/17", "/item/{user}/{id}", {"jba", "17"}},
                                    {"GET", "", "/item/jba/new", "/item/{user}/new", {"jba"}},
                                    {"GET", "", "/item/", "/item/{$}", {}},
                                    {"GET", "", "/item/jba/17/line2", "/item/", {}},
                                    {"POST", "alt.com", "/item/jba", "POST alt.com/item/{user}", {"jba"}},
                                    {"GET", "alt.com", "/item/jba", "GET /item/{user}", {"jba"}},
                                    {"GET", "", "/item", "", {}},  // does not match
                                    {"GET", "", "/headwins", "GET /headwins", {}},
                                    {"HEAD", "", "/headwins", "HEAD /headwins", {}},  // HEAD is more specific than GET
                                    {"GET", "", "/path/to/file", "/path/{p...}", {"to/file"}},
                                    {"GET", "", "/path/*", "/path/{p...}", {"*"}},
                                });

    // A pattern ending in {$} should only match URLS with a trailing slash.
    std::string pat1 = "/a/b/{$}";
    run_match_tests(build_tree({pat1})->root, {
                                                  {"GET", "", "/a/b", "", {}},
                                                  {"GET", "", "/a/b/", pat1, {}},
                                                  {"GET", "", "/a/b/c", "", {}},
                                                  {"GET", "", "/a/b/c/d", "", {}},
                                              });

    // A pattern ending in a single wildcard should not match a trailing slash URL.
    std::string pat2 = "/a/b/{w}";
    run_match_tests(build_tree({pat2})->root, {
                                                  {"GET", "", "/a/b", "", {}},
                                                  {"GET", "", "/a/b/", "", {}},
                                                  {"GET", "", "/a/b/c", pat2, {"c"}},
                                                  {"GET", "", "/a/b/c/d", "", {}},
                                              });

    // A pattern ending in a multi wildcard should match both URLs.
    std::string pat3 = "/a/b/{w...}";
    run_match_tests(build_tree({pat3})->root, {
                                                  {"GET", "", "/a/b", "", {}},
                                                  {"GET", "", "/a/b/", pat3, {""}},
                                                  {"GET", "", "/a/b/c", pat3, {"c"}},
                                                  {"GET", "", "/a/b/c/d", pat3, {"c/d"}},
                                              });

    // All three of the above should work together.
    run_match_tests(build_tree({pat1, pat2, pat3})->root, {
                                                              {"GET", "", "/a/b", "", {}},
                                                              {"GET", "", "/a/b/", pat1, {}},
                                                              {"GET", "", "/a/b/c", pat2, {"c"}},
                                                              {"GET", "", "/a/b/c/d", pat3, {"c/d"}},
                                                          });
}

TEST(GoNetHttp, MatchingMethods) {
    auto host_tree = build_tree({"GET a.com/", "PUT b.com/", "POST /foo/{x}"});
    auto post_tree = build_tree({"POST /"});
    auto get_tree = build_tree({"GET /"});
    auto empty_tree = build_tree({"/"});
    struct {
        std::string name;
        const routing_node* tree;
        std::string host, path, want;
    } tests[] = {
        {"post", &post_tree->root, "", "/foo", "POST"},
        {"get", &get_tree->root, "", "/foo", "GET,HEAD"},
        {"host", &host_tree->root, "", "/foo", ""},
        {"host", &host_tree->root, "", "/foo/bar", "POST"},
        {"host2", &host_tree->root, "a.com", "/foo/bar", "GET,HEAD,POST"},
        {"host3", &host_tree->root, "b.com", "/bar", "PUT"},
        // This case shouldn't come up because we only call matchingMethods
        // when there was no match, but we include it for completeness.
        {"empty", &empty_tree->root, "", "/", ""},
    };
    for (auto& test : tests) {
        std::set<std::string> ms;
        test.tree->matching_methods(test.host, string(test.path), ms);
        std::string got;
        for (auto& m : ms) got += (got.empty() ? "" : ",") + m;
        EXPECT_EQ(got, test.want) << test.name;
    }
}

// ---------------------------------------------------------------------------
// server_test.Go

namespace {

// A handler identified by a number (Go's handler{i}).
struct numbered final : http::Handler {
    explicit numbered(int i) : i(i) {}
    void ServeHTTP(http::ResponseWriter&, Request&) override {}
    int i;
};

// Describes a handler as Go's %#v would, for the patterns of the tests.
std::string describe(const ptr<http::Handler>& h) {
    if (auto* n = dynamic_cast<numbered*>(h.get())) return fmt::Sprint("&http.handler{{i:{}}}", n->i).str();
    if (auto* r = dynamic_cast<RedirectHandler*>(h.get())) {
        return fmt::Sprint("&http.redirectHandler{{url:\"{}\", code:{}}}", r->url, r->code).str();
    }
    if (dynamic_cast<http::HandlerFunc*>(h.get())) return "(http.HandlerFunc)(0x0)";
    return "?";
}

ptr<Request> request(std::string_view method, std::string_view host, std::string_view path) {
    auto r = make_ptr<Request>();
    r->Method = string(method);
    r->Host = string(host);
    r->URL.Path = string(path);
    return r;
}

}  // namespace

TEST(GoNetHttp, FindHandler) {
    http::ServeMux mux;
    std::pair<std::string, int> handlers[] = {{"/", 1}, {"/foo/", 2}, {"/foo", 3}, {"/bar/", 4}, {"//foo", 5}};
    for (auto& [pat, i] : handlers) mux.Handle(pat, make_ptr<numbered>(i));

    struct {
        std::string method, path, want_handler;
    } tests[] = {
        {"GET", "/", "&http.handler{i:1}"},
        {"GET", "//", R"(&http.redirectHandler{url:"/", code:307})"},
        {"GET", "/foo/../bar/./..//baz", R"(&http.redirectHandler{url:"/baz", code:307})"},
        {"GET", "/foo", "&http.handler{i:3}"},
        {"GET", "/foo/x", "&http.handler{i:2}"},
        {"GET", "/bar/x", "&http.handler{i:4}"},
        {"GET", "/bar", R"(&http.redirectHandler{url:"/bar/", code:307})"},
        {"CONNECT", "", R"(\(http.HandlerFunc\)\(.*\))"},
        {"CONNECT", "/", "&http.handler{i:1}"},
        {"CONNECT", "//", "&http.handler{i:1}"},
        {"CONNECT", "//foo", "&http.handler{i:5}"},
        {"CONNECT", "/foo/../bar/./..//baz", "&http.handler{i:2}"},
        {"CONNECT", "/foo", "&http.handler{i:3}"},
        {"CONNECT", "/foo/x", "&http.handler{i:2}"},
        {"CONNECT", "/bar/x", "&http.handler{i:4}"},
        {"CONNECT", "/bar", R"(&http.redirectHandler{url:"/bar/", code:307})"},
    };
    for (auto& test : tests) {
        auto r = request(test.method, "example.com", test.path);
        auto [h, pat] = mux.Handler(*r);
        std::string got = describe(h);
        // The patterns use regexp syntax only for the HandlerFunc case.
        bool ok = test.want_handler.starts_with("\\(") ? std::regex_match(got, std::regex(test.want_handler))
                                                       : got == test.want_handler;
        EXPECT_TRUE(ok) << test.method << " " << test.path << ": got " << got << ", want " << test.want_handler;
    }
}

TEST(GoNetHttp, EmptyServeMux) {
    // Verify that a ServeMux with nothing registered doesn't panic.
    http::ServeMux mux;
    auto r = request("GET", "example.com", "/");
    auto [h, p] = mux.Handler(*r);
    EXPECT_EQ(p, "");
}

TEST(GoNetHttp, RegisterErr) {
    http::ServeMux mux;
    auto h = make_ptr<numbered>(0);
    mux.Handle("/a", h);

    struct {
        std::string pattern;
        ptr<http::Handler> handler;
        std::string want_regexp;
    } tests[] = {
        {"", h, "invalid pattern"},
        {"/", nullptr, "nil handler"},
        {"/{x", h, R"(parsing "/\{x": at offset 1: bad wildcard segment)"},
        {"/a", h, R"(conflicts with pattern.* \(registered at .*go_net_http_mux_test.cpp:\d+)"},
    };
    for (auto& test : tests) {
        auto err = mux_access::register_err(mux, test.pattern, test.handler);
        ASSERT_TRUE(err) << test.pattern << ": got nil error";
        std::string g(err.Error().view());
        EXPECT_TRUE(std::regex_search(g, std::regex(test.want_regexp))) << "got " << g << "\nwant string matching "
                                                                         << test.want_regexp;
    }
    // HandlerFunc(nil): a HandleFunc with an empty function.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(mux.HandleFunc("/", nullptr), "nil handler");
}

TEST(GoNetHttp, ExactMatch) {
    struct {
        std::string pattern, path;
        bool want;
    } tests[] = {
        {"", "/a", false},
        {"/", "/a", false},
        {"/a", "/a", true},
        {"/a/{x...}", "/a/b", false},
        {"/a/{x}", "/a/b", true},
        {"/a/b/", "/a/b/", true},
        {"/a/b/{$}", "/a/b/", true},
        {"/a/", "/a/b/", false},
    };
    for (auto& test : tests) {
        std::unique_ptr<pattern> pat;
        routing_node node;
        const routing_node* n = nullptr;
        if (!test.pattern.empty()) {
            pat = must_parse_pattern(test.pattern);
            node.pat = pat.get();
            n = &node;
        }
        EXPECT_EQ(exact_match(n, test.path), test.want) << test.pattern << ", " << test.path;
    }
}

TEST(GoNetHttp, EscapedPathsAndPatterns) {
    struct {
        std::string pattern;
        std::vector<std::string> paths;  // paths that match the pattern
    } matches[] = {
        {"/a", {"/a", "/%61"}},                                   // matches a path that unescapes to "/a"
        {"/%62", {"/b", "/%62"}},                                 // patterns are unescaped by segment
        {"/%7B/%7D", {"/{/}", "/%7b/}", "/{/%7d", "/%7B/%7D"}},   // the only way to match '{' or '}'
        {"/%x", {"/%25x"}},                                       // patterns that don't unescape are left unchanged
    };
    // Go 1.21's behavior (GODEBUG=httpmuxgo121) isn't ported.
    http::ServeMux mux;
    for (auto& m : matches) mux.HandleFunc(m.pattern, [](http::ResponseWriter&, Request&) {});
    for (auto& m : matches) {
        for (auto& p : m.paths) {
            auto [u, err] = url::ParseRequestURI(p);
            ASSERT_FALSE(err) << err;
            Request req;
            req.URL = u;
            auto [h, got_pattern] = mux.Handler(req);
            EXPECT_EQ(got_pattern, m.pattern) << p;
        }
    }
}

TEST(GoNetHttp, CleanPath) {
    struct {
        std::string in, want;
    } tests[] = {
        {"//", "/"},
        {"/x", "/x"},
        {"//x", "/x"},
        {"x//", "/x/"},
        {"a//b/////c", "/a/b/c"},
        {"/foo/../bar/./..//baz", "/baz"},
    };
    for (auto& test : tests) EXPECT_EQ(clean_path(string(test.in)), test.want) << test.in;
}

// ---------------------------------------------------------------------------
// serve_test.Go

namespace {

ptr<http::Handler> serve(int code) {
    return make_ptr<http::HandlerFunc>([code](http::ResponseWriter& w, Request&) { w.WriteHeader(code); });
}

// checkQueryStringHandler: 200 if r.URL.RawQuery is the URL without the
// scheme and the query, 500 otherwise.
void check_query_string_handler(http::ResponseWriter& w, Request& r) {
    url::URL u = r.URL;
    u.Scheme = string::from_static("http");
    u.Host = r.Host;
    u.RawQuery = {};
    w.WriteHeader("http://" + r.URL.RawQuery == u.String() ? 200 : 500);
}

void register_serve_mux(http::ServeMux& mux) {
    mux.Handle("/dir/", serve(200));
    mux.Handle("/search", serve(201));
    mux.Handle("codesearch.google.com/search", serve(202));
    mux.Handle("codesearch.google.com/", serve(203));
    mux.Handle("example.com/", make_ptr<http::HandlerFunc>(check_query_string_handler));
    mux.Handle("/pkg/bar/extra%2fpath", serve(200));
}

}  // namespace

TEST(GoNetHttp, ServeMuxHandler) {
    http::ServeMux mux;
    register_serve_mux(mux);
    struct {
        std::string method, host, path;
        int code;
        std::string pattern;
    } tests[] = {
        {"GET", "google.com", "/", 404, ""},
        {"GET", "google.com", "/dir", 307, "/dir/"},
        {"GET", "google.com", "/dir/", 200, "/dir/"},
        {"GET", "google.com", "/dir/file", 200, "/dir/"},
        {"GET", "google.com", "/search", 201, "/search"},
        {"GET", "google.com", "/search/", 404, ""},
        {"GET", "google.com", "/search/foo", 404, ""},
        {"GET", "codesearch.google.com", "/search", 202, "codesearch.google.com/search"},
        {"GET", "codesearch.google.com", "/search/", 203, "codesearch.google.com/"},
        {"GET", "codesearch.google.com", "/search/foo", 203, "codesearch.google.com/"},
        {"GET", "codesearch.google.com", "/", 203, "codesearch.google.com/"},
        {"GET", "codesearch.google.com:443", "/", 203, "codesearch.google.com/"},
        {"GET", "images.google.com", "/search", 201, "/search"},
        {"GET", "images.google.com", "/search/", 404, ""},
        {"GET", "images.google.com", "/search/foo", 404, ""},
        {"GET", "google.com", "/../search", 307, "/search"},
        {"GET", "google.com", "/dir/..", 307, ""},
        {"GET", "google.com", "/dir/..", 307, ""},
        {"GET", "google.com", "/dir/./file", 307, "/dir/"},
    };
    for (auto& tt : tests) {
        auto r = request(tt.method, tt.host, tt.path);
        auto [h, pattern] = mux.Handler(*r);
        auto rr = httptest::NewRecorder();
        h->ServeHTTP(*rr, *r);
        EXPECT_TRUE(pattern == tt.pattern && rr->Code == tt.code)
            << tt.method << " " << tt.host << " " << tt.path << " = " << rr->Code << ", " << pattern << ", want " << tt.code
            << ", " << tt.pattern;
    }
}

TEST(GoNetHttp, ServeMuxHandlerTrailingSlash) {
    // Issue 73688
    http::ServeMux mux;
    const std::string original = "/{x}/";
    mux.Handle(original, http::NotFoundHandler());
    auto [r, err] = http::NewRequest("POST", "/foo");
    ASSERT_FALSE(err);
    auto [h, p] = mux.Handler(*r);
    EXPECT_EQ(p, original);
}

TEST(GoNetHttp, ServeMuxHandleFuncWithNilHandler) {
    // Issue 24297
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            http::ServeMux mux;
            mux.HandleFunc("/", nullptr);
        },
        "nil handler");
}

TEST(GoNetHttp, ServeMuxHandlerRedirects) {
    // Automatic redirects generated by mux.Handler() shouldn't clear the
    // request's query string.
    http::ServeMux mux;
    register_serve_mux(mux);
    struct {
        std::string method, host, url;
        int code;
        bool redir_ok;
    } tests[] = {
        {"GET", "google.com", "/", 404, false},
        {"GET", "example.com", "/test/?example.com/test/", 200, false},
        {"GET", "example.com", "test/?example.com/test/", 200, true},
        {"GET", "google.com", "/pkg/bar//extra%2fpath", 200, true},
        {"GET", "google.com", "/dir/b%2fc/..", 200, true},
        {"GET", "google.com", "/doesnotexist/b%2fc/..", 404, true},
    };
    for (auto& tt : tests) {
        int tries = 1;  // expect at most 1 redirection if redir_ok is true.
        std::string turl = tt.url;
        for (;;) {
            auto [u, e] = url::Parse(turl);
            ASSERT_FALSE(e) << e;
            Request r;
            r.Method = string(tt.method);
            r.Host = string(tt.host);
            r.URL = u;
            auto [h, p] = mux.Handler(r);
            auto rr = httptest::NewRecorder();
            h->ServeHTTP(*rr, r);
            if (rr->Code != 307) {
                EXPECT_EQ(rr->Code, tt.code) << tt.method << " " << tt.host << " " << tt.url;
                break;
            }
            if (!tt.redir_ok) {
                ADD_FAILURE() << tt.method << " " << tt.host << " " << tt.url << ", unexpected redirect";
                break;
            }
            turl = rr->Header().Get("Location").str();
            tries--;
        }
        EXPECT_GE(tries, 0) << tt.method << " " << tt.host << " " << tt.url << ", too many redirects";
    }
}

TEST(GoNetHttp, ServeMuxHandlerRedirectPost) {
    http::ServeMux mux;
    mux.HandleFunc("POST /test/", [](http::ResponseWriter& w, Request&) { w.WriteHeader(200); });
    int code = 0, retries;
    std::string start_url = "http://example.com/test";
    std::string req_url = start_url;
    for (retries = 0; retries <= 1; retries++) {
        auto r = httptest::NewRequest("POST", req_url, strings::NewReader("hello world"));
        auto [h, p] = mux.Handler(*r);
        auto rr = httptest::NewRecorder();
        h->ServeHTTP(*rr, *r);
        code = rr->Code;
        if (code == 307) {
            req_url = rr->Result()->Header.Get("Location").str();
            continue;
        }
        if (code != 200) ADD_FAILURE() << "unhandled response code: " << code;
    }
    EXPECT_EQ(code, 200) << "POST " << start_url << " after " << retries << " retries";
}

TEST(GoNetHttp, MuxRedirectLeadingSlashes) {
    // Tests for https://golang.org/issue/900
    for (std::string path : {"//foo.txt", "///foo.txt", "/../../foo.txt"}) {
        auto br = make_ptr<bufio::Reader>(strings::NewReader(string("GET " + path + " HTTP/1.1\r\nHost: test\r\n\r\n")));
        auto [req, err] = read_request(br, http::DefaultMaxHeaderBytes);
        ASSERT_FALSE(err) << err;
        http::ServeMux mux;
        auto resp = httptest::NewRecorder();
        mux.ServeHTTP(*resp, *req);
        EXPECT_EQ(resp->Header().Get("Location"), "/foo.txt") << path;
        EXPECT_EQ(resp->Code, http::StatusTemporaryRedirect) << path;
    }
}

TEST(GoNetHttp, ServeWithSlashRedirectKeepsQueryString) {
    // The "/route" redirect implicitly created by a registered "/route/"
    // keeps the query string. See Issue 17841.
    auto write_back_query = [](http::ResponseWriter& w, Request& r) { fmt::Print(w, "{}", r.URL.RawQuery); };
    auto mux = http::NewServeMux();
    mux->HandleFunc("/testOne", write_back_query);
    mux->HandleFunc("/testTwo/", write_back_query);
    mux->HandleFunc("/testThree", write_back_query);
    mux->HandleFunc("/testThree/", [](http::ResponseWriter& w, Request& r) { fmt::Print(w, "{}:bar", r.URL.RawQuery); });
    auto ts = httptest::NewServer(mux);

    struct {
        std::string path, method, want;
        bool status_ok;
    } tests[] = {
        {"/testOne?this=that", "GET", "this=that", true},
        {"/testTwo?foo=bar", "GET", "foo=bar", true},
        {"/testTwo?a=1&b=2&a=3", "GET", "a=1&b=2&a=3", true},
        {"/testTwo?", "GET", "", true},
        {"/testThree?foo", "GET", "foo", true},
        {"/testThree/?foo", "GET", "foo:bar", true},
        {"/testThree?foo", "CONNECT", "foo", true},
        {"/testThree/?foo", "CONNECT", "foo:bar", true},

        // canonicalization or not
        {"/testOne/foo/..?foo", "GET", "foo", true},
        {"/testOne/foo/..?foo", "CONNECT", "404 page not found\n", false},
    };
    int i = 0;
    for (auto& tt : tests) {
        auto [req, rerr] = http::NewRequest(tt.method, std::string(ts->URL.view()) + tt.path);
        ASSERT_FALSE(rerr) << rerr;
        auto [res, err] = ts->Client().Do(req);
        if (err) {
            i++;
            continue;
        }
        auto [slurp, _] = io::ReadAll(*res->Body);
        res->Body->Close();
        if (!tt.status_ok) {
            EXPECT_EQ(res->StatusCode, 404) << "#" << i;
        }
        EXPECT_EQ(as_string_view(slurp), tt.want) << "#" << i;
        i++;
    }
}

// ---------------------------------------------------------------------------
// request_test.Go

TEST(GoNetHttp, PathValueNoMatch) {
    // PathValue and SetPathValue work on a Request that was never matched.
    Request r;
    EXPECT_EQ(r.PathValue("x"), "");
    r.SetPathValue("x", "a");
    EXPECT_EQ(r.PathValue("x"), "a");
}

TEST(GoNetHttp, PathValueAndPattern) {
    struct {
        std::string pattern, url;
        std::map<std::string, std::string> want;
    } tests[] = {
        {"/{a}/is/{b}/{c...}", "/now/is/the/time/for/all", {{"a", "now"}, {"b", "the"}, {"c", "time/for/all"}, {"d", ""}}},
        {"/names/{name}/{other...}", "/names/%2fjohn/address", {{"name", "/john"}, {"other", "address"}}},
        {"/names/{name}/{other...}", "/names/john%2Fdoe/there/is%2F/more", {{"name", "john/doe"}, {"other", "there/is//more"}}},
        {"/names/{name}/{other...}", "/names/n/*", {{"name", "n"}, {"other", "*"}}},
    };
    for (auto& test : tests) {
        auto mux = http::NewServeMux();
        bool called = false;
        mux->HandleFunc(test.pattern, [&](http::ResponseWriter&, Request& r) {
            called = true;
            for (auto& [name, want] : test.want) EXPECT_EQ(r.PathValue(name), want) << test.pattern << ", " << name;
            EXPECT_EQ(r.Pattern, test.pattern);
        });
        auto server = httptest::NewServer(mux);
        auto [res, err] = http::Get(std::string(server->URL.view()) + test.url);
        ASSERT_FALSE(err) << err;
        res->Body->Close();
        EXPECT_TRUE(called) << test.url;
    }
}

TEST(GoNetHttp, SetPathValue) {
    auto mux = http::NewServeMux();
    mux->HandleFunc("/a/{b}/c/{d...}", [](http::ResponseWriter&, Request& r) {
        std::map<std::string, std::string> kvs = {{"b", "X"}, {"d", "Y"}, {"a", "Z"}};
        for (auto& [k, v] : kvs) r.SetPathValue(k, string(v));
        for (auto& [k, w] : kvs) EXPECT_EQ(r.PathValue(k), w);
    });
    auto server = httptest::NewServer(mux);
    auto [res, err] = http::Get(std::string(server->URL.view()) + "/a/b/c/d/e");
    ASSERT_FALSE(err) << err;
    res->Body->Close();
}
