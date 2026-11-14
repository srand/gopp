// ServeMux, after Go's net/http/pattern.Go, routing_tree.Go and server.Go.
#include "internal.hpp"
#include "routing.hpp"

#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace gopp::http {

namespace internal {

// ---------------------------------------------------------------------------
// Patterns

namespace {

string pathUnescapeOrRaw(std::string_view s) {
    if (s.find('%') == s.npos) return string(s);
    auto [u, err] = url::PathUnescape(s);
    return err ? string(s) : u;
}

bool validWildcardName(std::string_view s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
        bool digit = c >= '0' && c <= '9';
        if (!letter && c != '_' && (i == 0 || !digit)) return false;
    }
    return true;
}

}  // namespace

std::pair<std::unique_ptr<pattern>, gopp::error> parsePattern(std::string_view s) {
    if (s.empty()) return {nullptr, errors::New("empty pattern")};
    std::size_t off = 0;  // offset into s, for error messages
    auto fail = [&](std::string_view msg) {
        return std::pair<std::unique_ptr<pattern>, gopp::error>{nullptr, fmt::Errorf("at offset {}: {}", off, msg)};
    };

    // Method, then any spaces or tabs, then host and path.
    std::string_view method, rest = s;
    bool found = false;
    if (auto i = s.find_first_of(" \t"); i != s.npos) {
        method = s.substr(0, i);
        rest = s.substr(i + 1);
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) rest.remove_prefix(1);
        found = true;
    }
    if (!found) rest = s;
    if (!method.empty() && !validMethod(method)) return fail(fmt::Sprint("invalid method {}", strconv::Quote(method)).view());
    auto p = std::make_unique<pattern>();
    p->str = string(s);
    p->method = string(method);

    if (found) off = method.size() + 1;
    auto i = rest.find('/');
    if (i == rest.npos) return fail("host/path missing /");
    p->host = string(rest.substr(0, i));
    rest = rest.substr(i);
    if (auto j = p->host.find('{'); j != string::npos) {
        off += j;
        return fail("host contains '{' (missing initial '/'?)");
    }
    off += i;

    // At this point, rest is the path. A non-CONNECT pattern with an unclean
    // path can never match, because paths are cleaned before matching.
    if (!method.empty() && method != "CONNECT" && cleanPath(string(rest)).view() != rest) {
        return fail("non-CONNECT pattern with unclean path can never match");
    }

    std::set<std::string, std::less<>> seen;  // wildcard names, to catch duplicates
    while (!rest.empty()) {
        rest.remove_prefix(1);  // the slash
        off = s.size() - rest.size();
        if (rest.empty()) {
            // A trailing slash is an anonymous multi wildcard.
            p->segments.push_back({string(), true, true});
            break;
        }
        auto k = rest.find('/');
        if (k == rest.npos) k = rest.size();
        std::string_view seg = rest.substr(0, k);
        rest = rest.substr(k);
        auto brace = seg.find('{');
        if (brace == seg.npos) {
            // A literal segment; patterns are unescaped by segment.
            p->segments.push_back({pathUnescapeOrRaw(seg), false, false});
            continue;
        }
        if (brace != 0) return fail("bad wildcard segment (must start with '{')");
        if (seg.back() != '}') return fail("bad wildcard segment (must end with '}')");
        std::string_view name = seg.substr(1, seg.size() - 2);
        if (name == "$") {
            if (!rest.empty()) return fail("{$} not at end");
            p->segments.push_back({string::fromStatic("/"), false, false});
            break;
        }
        bool multi = name.ends_with("...");
        if (multi) name.remove_suffix(3);
        if (multi && !rest.empty()) return fail("{...} wildcard not at end");
        if (name.empty()) return fail("empty wildcard");
        if (!validWildcardName(name)) return fail(fmt::Sprint("bad wildcard name {}", strconv::Quote(name)).view());
        if (seen.contains(name)) return fail(fmt::Sprint("duplicate wildcard name {}", strconv::Quote(name)).view());
        seen.emplace(name);
        p->segments.push_back({string(name), true, multi});
    }
    return {std::move(p), nullptr};
}

std::string_view to_string(relationship r) noexcept {
    switch (r) {
    case relationship::equivalent: return "equivalent";
    case relationship::moreGeneral: return "moreGeneral";
    case relationship::moreSpecific: return "moreSpecific";
    case relationship::disjoint: return "disjoint";
    case relationship::overlaps: return "overlaps";
    }
    return "?";
}

relationship inverseRelationship(relationship r) noexcept {
    if (r == relationship::moreSpecific) return relationship::moreGeneral;
    if (r == relationship::moreGeneral) return relationship::moreSpecific;
    return r;
}

relationship combineRelationships(relationship r1, relationship r2) noexcept {
    switch (r1) {
    case relationship::equivalent: return r2;
    case relationship::disjoint: return relationship::disjoint;
    case relationship::overlaps: return r2 == relationship::disjoint ? relationship::disjoint : relationship::overlaps;
    case relationship::moreGeneral:
    case relationship::moreSpecific:
        if (r2 == relationship::equivalent) return r1;
        if (r2 == inverseRelationship(r1)) return relationship::overlaps;
        return r2;
    }
    return relationship::disjoint;
}

relationship compareMethods(const pattern& p1, const pattern& p2) {
    if (p1.method == p2.method) return relationship::equivalent;
    if (p1.method.empty()) return relationship::moreGeneral;  // p1 matches any method
    if (p2.method.empty()) return relationship::moreSpecific;
    if (p1.method == "GET" && p2.method == "HEAD") return relationship::moreGeneral;  // GET matches HEAD too
    if (p2.method == "GET" && p1.method == "HEAD") return relationship::moreSpecific;
    return relationship::disjoint;
}

namespace {

relationship compareSegments(const segment& s1, const segment& s2) {
    if (s1.multi && s2.multi) return relationship::equivalent;
    if (s1.multi) return relationship::moreGeneral;
    if (s2.multi) return relationship::moreSpecific;
    if (s1.wild && s2.wild) return relationship::equivalent;
    if (s1.wild) return s2.s == "/" ? relationship::disjoint : relationship::moreGeneral;  // {$} matches only "/"
    if (s2.wild) return s1.s == "/" ? relationship::disjoint : relationship::moreSpecific;
    return s1.s == s2.s ? relationship::equivalent : relationship::disjoint;
}

void writeSegment(std::string& b, const segment& s) {
    b.push_back('/');
    if (!s.multi && s.s != "/") b.append(s.s.view());
}

void writeMatchingPath(std::string& b, std::span<const segment> segs) {
    for (const segment& s : segs) writeSegment(b, s);
}

}  // namespace

relationship comparePaths(const pattern& p1, const pattern& p2) {
    // Different numbers of segments are disjoint unless a multi wildcard
    // at the end can take up the difference.
    if (p1.segments.size() != p2.segments.size() && !p1.lastSegment().multi && !p2.lastSegment().multi) {
        return relationship::disjoint;
    }
    relationship rel = relationship::equivalent;
    std::size_t i = 0;
    for (; i < p1.segments.size() && i < p2.segments.size(); i++) {
        rel = combineRelationships(rel, compareSegments(p1.segments[i], p2.segments[i]));
        if (rel == relationship::disjoint) return rel;
    }
    if (i == p1.segments.size() && i == p2.segments.size()) return rel;
    // One pattern is longer; the shorter one must end in a multi.
    if (p1.segments.size() < p2.segments.size() && p1.lastSegment().multi) {
        return combineRelationships(rel, relationship::moreGeneral);
    }
    if (p2.segments.size() < p1.segments.size() && p2.lastSegment().multi) {
        return combineRelationships(rel, relationship::moreSpecific);
    }
    return relationship::disjoint;
}

bool conflictsWith(const pattern& p1, const pattern& p2) {
    if (p1.host != p2.host) return false;  // a host pattern is always more specific
    relationship mrel = compareMethods(p1, p2);
    if (mrel == relationship::disjoint) return false;
    relationship rel = combineRelationships(mrel, comparePaths(p1, p2));
    return rel == relationship::equivalent || rel == relationship::overlaps;
}

string commonPath(const pattern& p1, const pattern& p2) {
    std::string b;
    std::size_t i = 0;
    for (; i < p1.segments.size() && i < p2.segments.size(); i++) {
        const segment& s1 = p1.segments[i];
        writeSegment(b, s1.wild ? p2.segments[i] : s1);
    }
    if (i < p1.segments.size()) writeMatchingPath(b, std::span(p1.segments).subspan(i));
    else if (i < p2.segments.size()) writeMatchingPath(b, std::span(p2.segments).subspan(i));
    return string(b);
}

string differencePath(const pattern& p1, const pattern& p2) {
    std::string b;
    std::size_t i = 0;
    for (; i < p1.segments.size() && i < p2.segments.size(); i++) {
        const segment& s1 = p1.segments[i];
        const segment& s2 = p2.segments[i];
        if (s1.multi && s2.multi) {
            // From here the patterns match the same paths: the difference
            // was earlier.
            b.push_back('/');
            return string(b);
        }
        if (s1.multi && !s2.multi) {
            // A trailing slash distinguishes them, unless s2 is {$}; then
            // any segment will do.
            b.push_back('/');
            if (s2.s == "/") b.append(s1.s.empty() ? std::string_view("x") : s1.s.view());
            return string(b);
        }
        if (!s1.multi && s2.multi) {
            writeSegment(b, s1);
        } else if (s1.wild && s2.wild) {
            writeSegment(b, s1);  // both match anything: use the first name
        } else if (s1.wild && !s2.wild) {
            // Any segment other than the literal s2.s.
            if (s1.s != s2.s) {
                writeSegment(b, s1);
            } else {
                b.push_back('/');
                b.append(s2.s.view());
                b.push_back('x');
            }
        } else if (!s1.wild && s2.wild) {
            writeSegment(b, s1);
        } else {
            // Both literals; overlapping patterns have the same one here.
            if (s1.s != s2.s) panic(fmt::Sprint("literals differ: {} and {}", strconv::Quote(s1.s), strconv::Quote(s2.s)).view());
            writeSegment(b, s1);
        }
    }
    if (i < p1.segments.size()) writeMatchingPath(b, std::span(p1.segments).subspan(i));
    else if (i < p2.segments.size()) writeMatchingPath(b, std::span(p2.segments).subspan(i));
    return string(b);
}

string describeConflict(const pattern& p1, const pattern& p2) {
    relationship mrel = compareMethods(p1, p2);
    relationship prel = comparePaths(p1, p2);
    relationship rel = combineRelationships(mrel, prel);
    if (rel == relationship::equivalent) return fmt::Sprint("{} matches the same requests as {}", p1.str, p2.str);
    if (rel != relationship::overlaps) panic("describeConflict called with non-conflicting patterns");
    if (prel == relationship::overlaps) {
        return fmt::Sprint(
            "{0} and {1} both match some paths, like {2}.\nBut neither is more specific than the other.\n"
            "{0} matches {3}, but {1} doesn't.\n{1} matches {4}, but {0} doesn't.",
            p1.str, p2.str, strconv::Quote(commonPath(p1, p2)), strconv::Quote(differencePath(p1, p2)),
            strconv::Quote(differencePath(p2, p1)));
    }
    if (mrel == relationship::moreGeneral && prel == relationship::moreSpecific) {
        return fmt::Sprint("{} matches more methods than {}, but has a more specific path pattern", p1.str, p2.str);
    }
    if (mrel == relationship::moreSpecific && prel == relationship::moreGeneral) {
        return fmt::Sprint("{} matches fewer methods than {}, but has a more general path pattern", p1.str, p2.str);
    }
    return fmt::Sprint("bug: unexpected way for two patterns {} and {} to conflict: methods {}, paths {}", p1.str, p2.str,
                       to_string(mrel), to_string(prel));
}

// ---------------------------------------------------------------------------
// Routing tree

namespace {

// The first segment of path as a view: "/a/b" -> "a", "/b".
std::pair<std::string_view, std::string_view> firstSegmentView(std::string_view path) {
    if (path == "/") return {"/", {}};
    path.remove_prefix(1);
    auto i = path.find('/');
    if (i == path.npos) i = path.size();
    return {path.substr(0, i), path.substr(i)};
}

}  // namespace

std::pair<string, std::string_view> firstSegment(std::string_view path) {
    auto [seg, rest] = firstSegmentView(path);
    return {seg == "/" ? string::fromStatic("/") : pathUnescapeOrRaw(seg), rest};
}

void routingNode::set(const pattern* p, ptr<http::Handler> h) {
    if (pat || handler) panic("non-nil leaf fields");
    pat = p;
    handler = std::move(h);
}

routingNode* routingNode::addChild(const string& key) {
    if (key.empty()) {
        if (!emptyChild) emptyChild = std::make_unique<routingNode>();
        return emptyChild.get();
    }
    if (routingNode* c = findChild(key)) return c;
    children.emplace_back(key, std::make_unique<routingNode>());
    return children.back().second.get();
}

routingNode* routingNode::findChild(std::string_view key) const {
    if (key.empty()) return emptyChild.get();
    for (auto& [k, c] : children) {
        if (k == key) return c.get();
    }
    return nullptr;
}

void routingNode::addSegments(std::span<const segment> segs, const pattern* p, ptr<http::Handler> h) {
    if (segs.empty()) {
        set(p, std::move(h));
        return;
    }
    const segment& seg = segs[0];
    if (seg.multi) {
        if (segs.size() != 1) panic("multi wildcard not last");
        multiChild = std::make_unique<routingNode>();
        multiChild->set(p, std::move(h));
    } else if (seg.wild) {
        addChild(string())->addSegments(segs.subspan(1), p, std::move(h));
    } else {
        addChild(seg.s)->addSegments(segs.subspan(1), p, std::move(h));
    }
}

void routingNode::addPattern(const pattern* p, ptr<http::Handler> h) {
    // Keys of the first two levels: host, then method; "" is any.
    routingNode* n = addChild(p->host);
    n = n->addChild(p->method);
    n->addSegments(p->segments, p, std::move(h));
}

// path is a suffix of full, so that wildcard values can share its buffer.
const routingNode* routingNode::matchPath(const string& full, std::string_view path, matchesT& m) const {
    // An empty path matches the node itself, if it ends a pattern.
    if (path.empty()) return pat ? this : nullptr;
    auto [seg, rest] = firstSegmentView(path);
    string useg = seg == "/" ? string::fromStatic("/")
                  : seg.find('%') == seg.npos ? full.Sub(seg)
                                              : pathUnescapeOrRaw(seg);
    // Literal children first, as they are more specific.
    if (const routingNode* c = findChild(useg)) {
        std::size_t before = m.size();
        if (auto* n = c->matchPath(full, rest, m)) return n;
        m.resize(before);
    }
    // Then a single wildcard, which doesn't match the "/" of a trailing slash.
    if (seg != "/" && emptyChild) {
        std::size_t before = m.size();
        m.push_back(useg);
        if (auto* n = emptyChild->matchPath(full, rest, m)) return n;
        m.resize(before);
    }
    // Then a multi wildcard, which takes the rest of the path.
    if (multiChild) {
        if (!multiChild->pat->lastSegment().s.empty()) m.push_back(pathUnescapeOrRaw(path.substr(1)));
        return multiChild.get();
    }
    return nullptr;
}

const routingNode* routingNode::matchMethodAndPath(std::string_view method, const string& path, matchesT& m) const {
    if (auto* c = findChild(method)) {
        if (auto* n = c->matchPath(path, path, m)) return n;
    }
    if (method == "HEAD") {
        // GET patterns match HEAD requests too.
        if (auto* c = findChild("GET")) {
            m.clear();
            if (auto* n = c->matchPath(path, path, m)) return n;
        }
    }
    m.clear();
    if (emptyChild) return emptyChild->matchPath(path, path, m);
    return nullptr;
}

const routingNode* routingNode::match(std::string_view host, std::string_view method, const string& path,
                                        matchesT& m) const {
    if (!host.empty()) {
        if (auto* h = findChild(host)) {
            if (auto* n = h->matchMethodAndPath(method, path, m)) return n;
        }
    }
    m.clear();
    if (emptyChild) return emptyChild->matchMethodAndPath(method, path, m);
    return nullptr;
}

void routingNode::matchingMethods(std::string_view host, const string& path, std::set<std::string>& methods) const {
    auto add = [&](const routingNode* n) {
        if (!n) return;
        for (auto& [method, c] : n->children) {
            matchesT m;
            if (c->matchPath(path, path, m)) methods.emplace(method.view());
        }
    };
    if (!host.empty()) add(findChild(host));
    add(emptyChild.get());
    if (methods.contains("GET")) methods.emplace("HEAD");
}

bool exactMatch(const routingNode* n, std::string_view path) {
    if (!n) return false;
    // Without a multi, the match is exact.
    if (!n->pat->lastSegment().multi) return true;
    // A path without a trailing slash makes the multi match non-empty.
    if (!path.empty() && path.back() != '/') return false;
    // The match is exact if the pattern has as many segments as the path
    // has slashes.
    return static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) == n->pat->segments.size();
}

}  // namespace internal

// ---------------------------------------------------------------------------
// ServeMux

using internal::pattern;
using internal::routingNode;

namespace {

string stripHostPort(const string& h) {
    if (h.find(':') == string::npos) return h;
    auto [host, port, err] = net::SplitHostPort(h);
    return err ? h : host;
}

// A URL from an escaped path and a query, as Go's urlFromEscaped.
string urlFromEscaped(const string& escaped, const string& rawQuery) {
    url::URL u;
    auto [p, err] = url::PathUnescape(escaped);
    u.Path = err ? escaped : p;
    u.RawPath = escaped;
    u.RawQuery = rawQuery;
    return u.String();
}

struct methodNotAllowed final : Handler {
    explicit methodNotAllowed(string allow) : allow(std::move(allow)) {}
    void ServeHTTP(ResponseWriter& w, Request&) override {
        w.Header().Set("Allow", allow);
        http::Error(w, StatusText(405), 405);
    }
    string allow;
};

}  // namespace

struct ServeMux::impl {
    struct registration {
        std::unique_ptr<pattern> pat;
        ptr<http::Handler> handler;
    };

    std::mutex mu;  // serializes registration
    std::vector<registration> regs;
    // Lookups read the current tree without locking. Registration builds a
    // new tree and publishes it; old trees live as long as the mux.
    std::atomic<routingNode*> tree{nullptr};
    std::vector<std::unique_ptr<routingNode>> trees;

    gopp::error registerErr(std::string_view patstr, ptr<http::Handler> h, std::source_location loc) {
        if (patstr.empty()) return errors::New("http: invalid pattern");
        if (!h) return errors::New("http: nil handler");
        auto [pat, err] = internal::parsePattern(patstr);
        if (err) return fmt::Errorf("parsing {}: {}", strconv::Quote(patstr), err);
        pat->loc = fmt::Sprint("{}:{}", loc.file_name(), loc.line());

        std::lock_guard l(mu);
        for (auto& r : regs) {
            if (internal::conflictsWith(*pat, *r.pat)) {
                return fmt::Errorf("pattern {} (registered at {}) conflicts with pattern {} (registered at {}):\n{}",
                                   strconv::Quote(pat->str), pat->loc, strconv::Quote(r.pat->str), r.pat->loc,
                                   internal::describeConflict(*pat, *r.pat));
            }
        }
        regs.push_back({std::move(pat), std::move(h)});
        auto t = std::make_unique<routingNode>();
        for (auto& r : regs) t->addPattern(r.pat.get(), r.handler);
        tree.store(t.get(), std::memory_order_release);
        trees.push_back(std::move(t));
        return nullptr;
    }

    // Matches, or finds the redirect from /path to /path/ when only the
    // latter matches (Go's matchOrRedirect). redirectTo is set to the
    // escaped path to redirect to; with nullptr, there is no redirect.
    const routingNode* matchOrRedirect(const string& host, std::string_view method, const string& path,
                                          routingNode::matchesT& m, string* redirectTo) const {
        const routingNode* t = tree.load(std::memory_order_acquire);
        if (!t) return nullptr;
        const routingNode* n = t->match(host, method, path, m);
        if (!internal::exactMatch(n, path) && redirectTo && !path.ends_with("/") && !path.empty()) {
            string p2 = path + "/";
            routingNode::matchesT m2;
            const routingNode* n2 = t->match(host, method, p2, m2);
            if (internal::exactMatch(n2, p2)) {
                *redirectTo = p2;
                return n2;
            }
        }
        return n;
    }
};

ServeMux::ServeMux() : impl_(std::make_unique<impl>()) {}
ServeMux::ServeMux(ServeMux&& o) noexcept : object(), impl_(std::move(o.impl_)) {}
ServeMux& ServeMux::operator=(ServeMux&& o) noexcept {
    impl_ = std::move(o.impl_);
    return *this;
}
ServeMux::~ServeMux() = default;

void ServeMux::Handle(std::string_view pattern, ptr<http::Handler> h, std::source_location loc) {
    if (gopp::error err = impl_->registerErr(pattern, std::move(h), loc)) panic(err.Error().view());
}

std::pair<ptr<http::Handler>, string> ServeMux::Handler(Request& r) const {
    routingNode::matchesT matches;
    const routingNode* n = nullptr;
    string escaped = r.URL.EscapedPath();
    string path = escaped;
    string host;
    if (r.Method == "CONNECT") {
        // The /tree -> /tree/ redirect applies to CONNECT requests, but the
        // path cleaning doesn't.
        string redirectTo;
        impl_->matchOrRedirect(r.URL.Host, r.Method, path, matches, &redirectTo);
        if (!redirectTo.empty()) {
            string u = urlFromEscaped(redirectTo, r.URL.RawQuery);
            auto [unescaped, uerr] = url::PathUnescape(redirectTo);
            return {make_ptr<internal::RedirectHandler>(u, StatusTemporaryRedirect), uerr ? redirectTo : unescaped};
        }
        // Match again with r.Host, without redirecting.
        matches.clear();
        host = r.Host;
        n = impl_->matchOrRedirect(r.Host, r.Method, path, matches, nullptr);
    } else {
        // Paths are cleaned, and hosts stripped of their port, before
        // matching; requests for unclean paths are redirected.
        host = stripHostPort(r.Host);
        path = internal::cleanPath(path);
        string redirectTo;
        n = impl_->matchOrRedirect(host, r.Method, path, matches, &redirectTo);
        if (!redirectTo.empty()) {
            return {make_ptr<internal::RedirectHandler>(urlFromEscaped(redirectTo, r.URL.RawQuery),
                                                         StatusTemporaryRedirect),
                    n->pat->str};
        }
        if (path != escaped) {
            string pat = n ? n->pat->str : string();
            return {make_ptr<internal::RedirectHandler>(urlFromEscaped(path, r.URL.RawQuery), StatusTemporaryRedirect),
                    pat};
        }
    }
    if (!n) {
        // 405 if the path matches with other methods, else 404.
        std::set<std::string> methods;
        if (const routingNode* t = impl_->tree.load(std::memory_order_acquire)) t->matchingMethods(host, path, methods);
        if (!methods.empty()) {
            std::string allow;
            for (auto& m : methods) {
                if (!allow.empty()) allow += ", ";
                allow += m;
            }
            return {make_ptr<methodNotAllowed>(string(allow)), string()};
        }
        return {NotFoundHandler(), string()};
    }

    // Record the wildcard values.
    std::size_t mi = 0;
    for (auto& seg : n->pat->segments) {
        if (seg.wild && !seg.s.empty() && mi < matches.size()) r.SetPathValue(seg.s, matches[mi++]);
    }
    return {n->handler, n->pat->str};
}

void ServeMux::ServeHTTP(ResponseWriter& w, Request& r) {
    if (r.RequestURI == "*") {
        if (r.ProtoAtLeast(1, 1)) w.Header().Set("Connection", "close");
        w.WriteHeader(StatusBadRequest);
        return;
    }
    auto [h, pat] = Handler(r);
    r.Pattern = pat;
    h->ServeHTTP(w, r);
}

ptr<ServeMux> NewServeMux() { return make_ptr<ServeMux>(); }

ServeMux& DefaultServeMux() {
    static ServeMux* m = [] {
        auto* mux = new ServeMux();
        mux->addRef();  // never freed
        return mux;
    }();
    return *m;
}

void Handle(std::string_view pattern, ptr<http::Handler> h, std::source_location loc) {
    DefaultServeMux().Handle(pattern, std::move(h), loc);
}

namespace internal {

gopp::error muxAccess::registerErr(ServeMux& mux, std::string_view pattern, ptr<http::Handler> h, std::source_location loc) {
    return mux.impl_->registerErr(pattern, std::move(h), loc);
}

const routingNode* muxAccess::tree(const ServeMux& mux) { return mux.impl_->tree.load(std::memory_order_acquire); }

}  // namespace internal

// ---------------------------------------------------------------------------
// Helper handlers

void Error(ResponseWriter& w, std::string_view msg, int code) {
    Header& h = w.Header();
    // Drop headers meant for the successful response.
    h.Del("Content-Length");
    h.Set("Content-Type", string::fromStatic("text/plain; charset=utf-8"));
    h.Set("X-Content-Type-Options", string::fromStatic("nosniff"));
    w.WriteHeader(code);
    fmt::Println(w, "{}", msg);
}

void NotFound(ResponseWriter& w, Request&) { Error(w, "404 page not found", StatusNotFound); }

ptr<http::Handler> NotFoundHandler() {
    static Handler* h = [] {
        auto* f = new HandlerFunc(NotFound);
        f->addRef();
        return f;
    }();
    return ptr<http::Handler>(h);
}

void Redirect(ResponseWriter& w, Request& r, std::string_view target, int code) {
    string u(target);
    // Make a relative path absolute against the request path.
    if (!u.empty() && u[0] != '/' && u.find("://") == string::npos) {
        std::string_view old = r.URL.Path;
        if (old.empty()) old = "/";
        auto dir = old.substr(0, old.rfind('/') + 1);
        bool trailing = u.ends_with("/");
        u = internal::cleanPath(string(dir) + u);
        if (trailing && !u.ends_with("/")) u = u + "/";
    }
    w.Header().Set("Location", u);
    if (r.Method == "GET" || r.Method == "HEAD") w.Header().Set("Content-Type", string::fromStatic("text/html; charset=utf-8"));
    w.WriteHeader(code);
    if (r.Method == "GET") fmt::Print(w, "<a href=\"{}\">{}</a>.\n", u, StatusText(code));
}

ptr<http::Handler> RedirectHandler(string url, int code) { return make_ptr<internal::RedirectHandler>(std::move(url), code); }

ptr<http::Handler> StripPrefix(string prefix, ptr<http::Handler> h) {
    if (prefix.empty()) return h;
    return make_ptr<HandlerFunc>([prefix, h](ResponseWriter& w, Request& r) {
        string p = strings::TrimPrefix(r.URL.Path, prefix);
        string rp = strings::TrimPrefix(r.URL.RawPath, prefix);
        if (p.size() < r.URL.Path.size() && (r.URL.RawPath.empty() || rp.size() < r.URL.RawPath.size())) {
            auto r2 = make_ptr<Request>(r);
            r2->URL.Path = p;
            r2->URL.RawPath = rp;
            h->ServeHTTP(w, *r2);
        } else {
            NotFound(w, r);
        }
    });
}

}  // namespace gopp::http
