// Ported from Go 1.27.3's path/path.go and path/match.go.
#include <gopp/path.hpp>

#include <gopp/unicode/utf8.hpp>

#include <string>
#include <vector>

namespace gopp::path {

const error ErrBadPattern = errors::New("syntax error in pattern");

namespace {

// Go's lazybuf: a path being cleaned, copied only when it differs.
struct lazybuf {
    std::string_view s;
    std::string buf;
    bool copied = false;
    std::size_t w = 0;

    char index(std::size_t i) const { return copied ? buf[i] : s[i]; }
    void append(char c) {
        if (!copied) {
            if (w < s.size() && s[w] == c) {
                w++;
                return;
            }
            buf.assign(s.size(), '\0');
            buf.replace(0, w, s.substr(0, w));
            copied = true;
        }
        buf[w++] = c;
    }
    string str() const { return copied ? string(std::string_view(buf).substr(0, w)) : string(s.substr(0, w)); }
};

}  // namespace

string Clean(std::string_view path) {
    if (path.empty()) return string::from_static(".");
    bool rooted = path[0] == '/';
    std::size_t n = path.size();
    // Invariants: reading from path; r is index of next byte to process.
    // writing to buf; w is index of next byte to write. dotdot is index in
    // buf where .. must stop, either because it is the leading slash or
    // it is a leading ../../.. prefix.
    lazybuf out;
    out.s = path;
    std::size_t r = 0, dotdot = 0;
    if (rooted) {
        out.append('/');
        r = dotdot = 1;
    }
    while (r < n) {
        if (path[r] == '/') {
            // empty path element
            r++;
        } else if (path[r] == '.' && (r + 1 == n || path[r + 1] == '/')) {
            // . element
            r++;
        } else if (path[r] == '.' && path[r + 1] == '.' && (r + 2 == n || path[r + 2] == '/')) {
            // .. element: remove to last /
            r += 2;
            if (out.w > dotdot) {
                // can backtrack
                out.w--;
                while (out.w > dotdot && out.index(out.w) != '/') out.w--;
            } else if (!rooted) {
                // cannot backtrack, but not rooted, so append .. element.
                if (out.w > 0) out.append('/');
                out.append('.');
                out.append('.');
                dotdot = out.w;
            }
        } else {
            // real path element; add slash if needed
            if ((rooted && out.w != 1) || (!rooted && out.w != 0)) out.append('/');
            // copy element
            for (; r < n && path[r] != '/'; r++) out.append(path[r]);
        }
    }
    // Turn empty string into "."
    if (out.w == 0) return string::from_static(".");
    return out.str();
}

std::pair<string, string> Split(std::string_view path) {
    auto i = path.rfind('/');
    std::size_t k = i == path.npos ? 0 : i + 1;
    return {string(path.substr(0, k)), string(path.substr(k))};
}

string Join(std::span<const std::string_view> elem) {
    std::size_t size = 0;
    for (auto e : elem) size += e.size();
    if (size == 0) return {};
    std::string buf;
    buf.reserve(size + elem.size() - 1);
    for (auto e : elem) {
        if (!buf.empty() || !e.empty()) {
            if (!buf.empty()) buf += '/';
            buf += e;
        }
    }
    return Clean(buf);
}

string Join(std::initializer_list<std::string_view> elem) {
    return Join(std::span<const std::string_view>(elem.begin(), elem.size()));
}

string Ext(std::string_view path) {
    for (std::size_t i = path.size(); i-- > 0 && path[i] != '/';) {
        if (path[i] == '.') return string(path.substr(i));
    }
    return {};
}

string Base(std::string_view path) {
    if (path.empty()) return string::from_static(".");
    // Strip trailing slashes.
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    // Find the last element
    if (auto i = path.rfind('/'); i != path.npos) path = path.substr(i + 1);
    // If empty now, it had only slashes.
    if (path.empty()) return string::from_static("/");
    return string(path);
}

bool IsAbs(std::string_view path) { return !path.empty() && path[0] == '/'; }

string Dir(std::string_view path) { return Clean(Split(path).first.view()); }

namespace {

// scanChunk gets the next segment of pattern, which is a non-star string
// possibly preceded by a star.
std::tuple<bool, std::string_view, std::string_view> scanChunk(std::string_view pattern) {
    bool star = false;
    while (!pattern.empty() && pattern[0] == '*') {
        pattern.remove_prefix(1);
        star = true;
    }
    bool inrange = false;
    for (std::size_t i = 0; i < pattern.size(); i++) {
        switch (pattern[i]) {
        case '\\':
            // error check handled in matchChunk: bad pattern.
            if (i + 1 < pattern.size()) i++;
            break;
        case '[': inrange = true; break;
        case ']': inrange = false; break;
        case '*':
            if (!inrange) return {star, pattern.substr(0, i), pattern.substr(i)};
            break;
        }
    }
    return {star, pattern, {}};
}

std::tuple<rune, std::string_view, error> getEsc(std::string_view chunk) {
    if (chunk.empty() || chunk[0] == '-' || chunk[0] == ']') return {0, chunk, ErrBadPattern};
    if (chunk[0] == '\\') {
        chunk.remove_prefix(1);
        if (chunk.empty()) return {0, chunk, ErrBadPattern};
    }
    auto [r, n] = utf8::DecodeRuneInString(chunk);
    error err;
    if (r == utf8::RuneError && n == 1) err = ErrBadPattern;
    std::string_view nchunk = chunk.substr(std::size_t(n));
    if (nchunk.empty()) err = ErrBadPattern;
    return {r, nchunk, err};
}

// matchChunk checks whether chunk matches the beginning of s. If so, it
// returns the remainder of s (after the match). Chunk is all single-
// character operators: literals, char classes, and ?.
std::tuple<std::string_view, bool, error> matchChunk(std::string_view chunk, std::string_view s) {
    // failed records whether the match has failed. After the match fails,
    // the loop continues on processing chunk, checking that the pattern is
    // well-formed but no longer reading s.
    bool failed = false;
    while (!chunk.empty()) {
        failed = failed || s.empty();
        switch (chunk[0]) {
        case '[': {
            // character class
            rune r = 0;
            if (!failed) {
                auto [rr, n] = utf8::DecodeRuneInString(s);
                r = rr;
                s.remove_prefix(std::size_t(n));
            }
            chunk.remove_prefix(1);
            // possibly negated
            bool negated = false;
            if (!chunk.empty() && chunk[0] == '^') {
                negated = true;
                chunk.remove_prefix(1);
            }
            // parse all ranges
            bool match = false;
            int nrange = 0;
            for (;;) {
                if (!chunk.empty() && chunk[0] == ']' && nrange > 0) {
                    chunk.remove_prefix(1);
                    break;
                }
                auto [lo, c1, err] = getEsc(chunk);
                if (err) return {{}, false, err};
                chunk = c1;
                rune hi = lo;
                if (chunk[0] == '-') {
                    auto [h, c2, err2] = getEsc(chunk.substr(1));
                    if (err2) return {{}, false, err2};
                    hi = h;
                    chunk = c2;
                }
                match = match || (lo <= r && r <= hi);
                nrange++;
            }
            failed = failed || match == negated;
            break;
        }
        case '?':
            if (!failed) {
                failed = s[0] == '/';
                auto [r, n] = utf8::DecodeRuneInString(s);
                s.remove_prefix(std::size_t(n));
            }
            chunk.remove_prefix(1);
            break;
        case '\\':
            chunk.remove_prefix(1);
            if (chunk.empty()) return {{}, false, ErrBadPattern};
            [[fallthrough]];
        default:
            if (!failed) {
                failed = chunk[0] != s[0];
                s.remove_prefix(1);
            }
            chunk.remove_prefix(1);
            break;
        }
    }
    if (failed) return {{}, false, nullptr};
    return {s, true, nullptr};
}

}  // namespace

std::pair<bool, error> Match(std::string_view pattern, std::string_view name) {
    while (!pattern.empty()) {
        auto [star, chunk, rest] = scanChunk(pattern);
        pattern = rest;
        if (star && chunk.empty()) {
            // Trailing * matches rest of string unless it has a /.
            return {name.find('/') == name.npos, nullptr};
        }
        // Look for match at current position.
        auto [t, ok, err] = matchChunk(chunk, name);
        // if we're the last chunk, make sure we've exhausted the name
        // otherwise we'll give a false result even if we could still match
        // using the star
        if (ok && (t.empty() || !pattern.empty())) {
            name = t;
            continue;
        }
        if (err) return {false, err};
        if (star) {
            // Look for match skipping i+1 bytes. Cannot skip /.
            bool found = false;
            for (std::size_t i = 0; i < name.size() && name[i] != '/'; i++) {
                auto [t2, ok2, err2] = matchChunk(chunk, name.substr(i + 1));
                if (ok2) {
                    // if we're the last chunk, make sure we exhausted the name
                    if (pattern.empty() && !t2.empty()) continue;
                    name = t2;
                    found = true;
                    break;
                }
                if (err2) return {false, err2};
            }
            if (found) continue;  // Go's continue Pattern
        }
        // Before returning false with no error, check that the remainder
        // of the pattern is syntactically valid.
        while (!pattern.empty()) {
            auto [s2, chunk2, rest2] = scanChunk(pattern);
            pattern = rest2;
            if (auto [t3, ok3, err3] = matchChunk(chunk2, ""); err3) return {false, err3};
        }
        return {false, nullptr};
    }
    return {name.empty(), nullptr};
}

}  // namespace gopp::path
