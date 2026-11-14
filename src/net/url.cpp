// After Go's net/url/url.Go.
#include <gopp/net/url.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <string>

namespace gopp::url {

namespace {

enum class encoding { path, pathSegment, queryComponent, fragment };

bool ishex(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int unhex(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

bool shouldEscape(unsigned char c, encoding mode) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return false;
    switch (c) {
    case '-': case '_': case '.': case '~': return false;
    case '$': case '&': case '+': case ',': case '/': case ':': case ';': case '=': case '?': case '@':
        switch (mode) {
        case encoding::path: return c == '?';
        case encoding::pathSegment: return c == '/' || c == ';' || c == ',' || c == '?';
        case encoding::queryComponent: return true;
        case encoding::fragment: return false;
        }
    }
    if (mode == encoding::fragment) {
        switch (c) {
        case '!': case '(': case ')': case '*': return false;
        }
    }
    return true;
}

std::pair<string, error> unescape(std::string_view s, encoding mode) {
    std::size_t n = 0;
    bool hasPlus = false;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '%') {
            n++;
            if (i + 2 >= s.size() || !ishex(s[i + 1]) || !ishex(s[i + 2])) {
                std::string_view bad = s.substr(i, std::min<std::size_t>(3, s.size() - i));
                return {{}, errors::New("invalid URL escape " + strconv::Quote(bad))};
            }
            i += 3;
        } else {
            hasPlus = hasPlus || s[i] == '+';
            i++;
        }
    }
    if (n == 0 && !(hasPlus && mode == encoding::queryComponent)) return {string(s), nullptr};
    std::string out;
    out.reserve(s.size() - 2 * n);
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '%') {
            out.push_back(static_cast<char>(unhex(s[i + 1]) << 4 | unhex(s[i + 2])));
            i += 2;
        } else if (c == '+' && mode == encoding::queryComponent) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return {string(out), nullptr};
}

string escape(std::string_view s, encoding mode) {
    std::size_t hex = 0, spaces = 0;
    for (unsigned char c : s) {
        if (shouldEscape(c, mode)) {
            if (c == ' ' && mode == encoding::queryComponent) spaces++;
            else hex++;
        }
    }
    if (hex == 0 && spaces == 0) return string(s);
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 2 * hex);
    for (unsigned char c : s) {
        if (c == ' ' && mode == encoding::queryComponent) {
            out.push_back('+');
        } else if (shouldEscape(c, mode)) {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 15]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return string(out);
}

// Splits "scheme:rest" off rawurl if it has a valid scheme.
std::pair<std::string_view, std::string_view> getScheme(std::string_view raw) {
    for (std::size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            if (i == 0) return {{}, raw};
            continue;
        }
        if (c == ':') {
            if (i == 0) return {{}, raw};
            return {raw.substr(0, i), raw.substr(i + 1)};
        }
        return {{}, raw};
    }
    return {{}, raw};
}

std::pair<URL, error> parseImpl(std::string_view rawurl, bool viaRequest) {
    URL u;
    auto fail = [&](string msg) {
        auto e = make_ptr<Error>();
        e->Op = "parse";
        e->URL = string(rawurl);
        e->Err = errors::New(std::move(msg));
        return std::pair<URL, error>{URL{}, error(e)};
    };
    for (char c : rawurl) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return fail("net/url: invalid control character in URL");
    }
    if (rawurl.empty() && viaRequest) return fail("empty url");
    if (rawurl == "*") {
        u.Path = string::fromStatic("*");
        return {u, nullptr};
    }

    auto [scheme, rest] = getScheme(rawurl);
    u.Scheme = strings::ToLower(scheme);

    if (auto q = rest.find('?'); q != rest.npos) {
        u.RawQuery = string(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }

    if (!rest.starts_with('/')) {
        if (!u.Scheme.empty()) {
            u.Opaque = string(rest);
            return {u, nullptr};
        }
        if (viaRequest) return fail("invalid URI for request");
    }

    if ((!u.Scheme.empty() || !viaRequest) && rest.starts_with("//")) {
        std::string_view authority = rest.substr(2);
        std::size_t slash = authority.find('/');
        if (slash != authority.npos) {
            rest = authority.substr(slash);
            authority = authority.substr(0, slash);
        } else {
            rest = {};
        }
        if (auto at = authority.rfind('@'); at != authority.npos) authority = authority.substr(at + 1);  // drop userinfo
        u.Host = string(authority);
    }

    auto [p, err] = unescape(rest, encoding::path);
    if (err) return fail(err.Error());
    u.Path = p;
    // Keep the original encoding when it differs from ours.
    if (rest.find('%') != rest.npos && escape(p, encoding::path).view() != rest) u.RawPath = string(rest);
    return {u, nullptr};
}

}  // namespace

string Error::message() const { return Op + " " + strconv::Quote(URL) + ": " + Err.Error(); }

std::pair<string, error> PathUnescape(std::string_view s) { return unescape(s, encoding::pathSegment); }
std::pair<string, error> QueryUnescape(std::string_view s) { return unescape(s, encoding::queryComponent); }
string PathEscape(std::string_view s) { return escape(s, encoding::pathSegment); }
string QueryEscape(std::string_view s) { return escape(s, encoding::queryComponent); }

std::pair<URL, error> Parse(std::string_view rawurl) {
    std::string_view frag;
    if (auto h = rawurl.find('#'); h != rawurl.npos) {
        frag = rawurl.substr(h + 1);
        rawurl = rawurl.substr(0, h);
    }
    auto [u, err] = parseImpl(rawurl, false);
    if (err) return {u, err};
    if (!frag.empty()) {
        auto [f, ferr] = unescape(frag, encoding::fragment);
        if (ferr) return {u, ferr};
        u.Fragment = f;
    }
    return {u, nullptr};
}

std::pair<URL, error> ParseRequestURI(std::string_view rawurl) { return parseImpl(rawurl, true); }

string URL::EscapedPath() const {
    if (!RawPath.empty()) return RawPath;
    if (Path == "*") return Path;
    return escape(Path, encoding::path);
}

string URL::RequestURI() const {
    string result = Opaque;
    if (result.empty()) {
        result = EscapedPath();
        if (result.empty()) result = string::fromStatic("/");
    } else if (result.starts_with("//")) {
        result = Scheme + ":" + result;
    }
    if (!RawQuery.empty()) result = result + "?" + RawQuery;
    return result;
}

string URL::String() const {
    std::string s;
    if (!Scheme.empty()) {
        s += Scheme.view();
        s += ':';
    }
    if (!Opaque.empty()) {
        s += Opaque.view();
    } else {
        if (!Scheme.empty() || !Host.empty()) {
            s += "//";
            s += Host.view();
        }
        string ep = EscapedPath();
        if (!ep.empty() && ep[0] != '/' && !Host.empty()) s += '/';
        // RFC 3986 section 4.2: a relative path whose first segment has a
        // colon would read as a scheme; "./" keeps it a path.
        if (s.empty()) {
            std::string_view segment = ep.view().substr(0, ep.view().find('/'));
            if (segment.find(':') != segment.npos) s += "./";
        }
        s += ep.view();
    }
    if (!RawQuery.empty()) {
        s += '?';
        s += RawQuery.view();
    }
    if (!Fragment.empty()) {
        s += '#';
        s += escape(Fragment, encoding::fragment).view();
    }
    return string(s);
}

string URL::Hostname() const {
    std::string_view h = Host;
    if (h.starts_with('[')) {
        auto e = h.find(']');
        return e == h.npos ? string(h.substr(1)) : string(h.substr(1, e - 1));
    }
    auto c = h.rfind(':');
    return c == h.npos ? Host : Host.slice(0, c);
}

string URL::Port() const {
    std::string_view h = Host;
    auto c = h.rfind(':');
    if (c == h.npos || h.find(']', c) != h.npos) return {};
    return Host.slice(c + 1);
}

std::pair<Values, error> ParseQuery(std::string_view query) {
    Values v;
    error first;
    while (!query.empty()) {
        std::string_view key = query;
        auto amp = key.find('&');
        if (amp != key.npos) {
            query = key.substr(amp + 1);
            key = key.substr(0, amp);
        } else {
            query = {};
        }
        if (key.find(';') != key.npos) {
            if (!first) first = errors::New("invalid semicolon separator in query");
            continue;
        }
        if (key.empty()) continue;
        std::string_view value;
        if (auto eq = key.find('='); eq != key.npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
        }
        auto [k, kerr] = QueryUnescape(key);
        if (kerr) {
            if (!first) first = kerr;
            continue;
        }
        auto [val, verr] = QueryUnescape(value);
        if (verr) {
            if (!first) first = verr;
            continue;
        }
        v.Add(k, val);
    }
    return {v, first};
}

Values URL::Query() const { return ParseQuery(RawQuery).first; }

string Values::Get(std::string_view key) const {
    for (auto& [k, v] : kv_) {
        if (k == key) return v;
    }
    return {};
}

std::vector<string> Values::operator[](std::string_view key) const {
    std::vector<string> vs;
    for (const auto& [k, v] : kv_) {
        if (k == key) vs.push_back(v);
    }
    return vs;
}

void Values::Set(string key, string value) {
    Del(key);
    kv_.emplace_back(std::move(key), std::move(value));
}

void Values::Add(string key, string value) { kv_.emplace_back(std::move(key), std::move(value)); }

void Values::Del(std::string_view key) {
    std::erase_if(kv_, [&](auto& kv) { return kv.first == key; });
}

bool Values::Has(std::string_view key) const {
    return std::any_of(kv_.begin(), kv_.end(), [&](auto& kv) { return kv.first == key; });
}

string Values::Encode() const {
    std::vector<std::pair<string, string>> sorted = kv_;
    std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::string out;
    for (auto& [k, v] : sorted) {
        if (!out.empty()) out += '&';
        out += QueryEscape(k).view();
        out += '=';
        out += QueryEscape(v).view();
    }
    return string(out);
}

}  // namespace gopp::url
