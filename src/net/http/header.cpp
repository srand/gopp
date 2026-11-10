// After Go's net/http/header.Go and net/textproto.
#include "internal.hpp"

#include <gopp/strings.hpp>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace gopp::http {

namespace internal {

bool is_token_char(unsigned char c) noexcept {
    // tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
    //         "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != 0;
}

bool valid_header_field_name(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!is_token_char(c)) return false;
    }
    return true;
}

bool valid_header_field_value(std::string_view s) noexcept {
    for (unsigned char c : s) {
        // CTLs except HTAB are invalid; obs-text (>= 0x80) is allowed.
        if ((c < ' ' && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

bool valid_method(std::string_view m) noexcept { return valid_header_field_name(m); }

bool has_token(std::string_view v, std::string_view token) noexcept {
    if (token.size() > v.size() || token.empty()) return false;
    while (!v.empty()) {
        auto comma = v.find(',');
        std::string_view part = v.substr(0, comma);
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.remove_suffix(1);
        if (strings::EqualFold(part, token)) return true;
        if (comma == v.npos) break;
        v.remove_prefix(comma + 1);
    }
    return false;
}

void append_date(std::string& out) {
    // The formatted date is cached per thread and second.
    thread_local time_t cached_sec = 0;
    thread_local char cached[32];
    thread_local std::size_t cached_len = 0;
    time_t now = ::time(nullptr);
    if (now != cached_sec) {
        string s = time::Time::Unix(now, 0).FormatHTTP();
        cached_len = std::min(s.size(), sizeof(cached));
        std::memcpy(cached, s.data(), cached_len);
        cached_sec = now;
    }
    out.append(cached, cached_len);
}

}  // namespace internal

namespace {

constexpr char to_lower_c(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
constexpr char to_upper_c(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

bool is_canonical(std::string_view k) noexcept {
    bool upper = true;
    for (char c : k) {
        if (upper && c >= 'a' && c <= 'z') return false;
        if (!upper && c >= 'A' && c <= 'Z') return false;
        upper = c == '-';
    }
    return true;
}

}  // namespace

string CanonicalHeaderKey(std::string_view k) {
    // Keys with invalid characters are returned unchanged, as in Go.
    for (unsigned char c : k) {
        if (!internal::is_token_char(c)) return string(k);
    }
    if (is_canonical(k)) return string(k);
    return string::build(k.size(), [&](char* p) {
        bool upper = true;
        for (std::size_t i = 0; i < k.size(); i++) {
            p[i] = upper ? to_upper_c(k[i]) : to_lower_c(k[i]);
            upper = k[i] == '-';
        }
    });
}

string Header::Get(std::string_view key) const {
    for (auto& e : entries_) {
        if (strings::EqualFold(e.key, key)) return e.value;
    }
    return {};
}

std::vector<string> Header::Values(std::string_view key) const {
    std::vector<string> out;
    for (auto& e : entries_) {
        if (strings::EqualFold(e.key, key)) out.push_back(e.value);
    }
    return out;
}

bool Header::Has(std::string_view key) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](auto& e) { return strings::EqualFold(e.key, key); });
}

void Header::Set(std::string_view key, string value) {
    // Reuse the first entry of key and drop the others.
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](auto& e) { return strings::EqualFold(e.key, key); });
    if (it == entries_.end()) {
        entries_.push_back({CanonicalHeaderKey(key), std::move(value)});
        return;
    }
    it->value = std::move(value);
    auto out = it + 1;
    for (auto in = it + 1; in != entries_.end(); ++in) {
        if (!strings::EqualFold(in->key, key)) {
            if (out != in) *out = std::move(*in);
            ++out;
        }
    }
    entries_.erase(out, entries_.end());
}

void Header::Add(std::string_view key, string value) { entries_.push_back({CanonicalHeaderKey(key), std::move(value)}); }

void Header::Del(std::string_view key) {
    std::erase_if(entries_, [&](auto& e) { return strings::EqualFold(e.key, key); });
}

void Header::WriteTo(std::string& out, std::initializer_list<std::string_view> exclude) const {
    // Sorted by key, like Go's Header.Write; values of a key keep their order.
    std::size_t n = entries_.size();
    const entry* small[32];
    std::vector<const entry*> big;
    const entry** order = small;
    if (n > 32) {
        big.resize(n);
        order = big.data();
    }
    for (std::size_t i = 0; i < n; i++) order[i] = &entries_[i];
    std::stable_sort(order, order + n, [](const entry* a, const entry* b) { return a->key < b->key; });
    for (std::size_t i = 0; i < n; i++) {
        const entry& e = *order[i];
        bool skip = false;
        for (auto x : exclude) {
            if (strings::EqualFold(e.key, x)) {
                skip = true;
                break;
            }
        }
        if (skip) continue;
        out.append(e.key.view());
        out.append(": ");
        // Newlines in values would split the header: replace them.
        for (char c : e.value) out.push_back(c == '\r' || c == '\n' ? ' ' : c);
        out.append("\r\n");
    }
}

gopp::error Header::Write(io::Writer& w) const {
    std::string out;
    WriteTo(out);
    return io::WriteString(w, out).second;
}

// ---------------------------------------------------------------------------
// Status text and errors

std::string_view StatusText(int code) noexcept {
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Request Entity Too Large";
    case 414: return "Request URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Requested Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default: return "";
    }
}

const gopp::error ErrServerClosed = errors::New("http: Server closed");
const gopp::error ErrBodyNotAllowed = errors::New("http: request method or response status code does not allow body");
const gopp::error ErrContentLength = errors::New("http: wrote more than the declared Content-Length");
const gopp::error ErrBodyReadAfterClose = errors::New("http: invalid Read on closed Body");
const gopp::error ErrHandlerTimeout = errors::New("http: Handler timeout");
const gopp::error ErrAbortHandler = errors::New("net/http: abort Handler");

namespace internal {
const gopp::error err_header_too_large = errors::New("http: request header too large");
}

}  // namespace gopp::http
