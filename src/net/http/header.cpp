// After Go's net/http/header.Go and net/textproto.
#include "internal.hpp"

#include <gopp/fmt.hpp>
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

// ---------------------------------------------------------------------------
// ParseTime: Go's time.Parse, for the three layouts of HTTP dates.

namespace {

constexpr std::string_view short_day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::string_view long_day_names[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                               "Thursday", "Friday", "Saturday"};
constexpr std::string_view short_month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// The elements of a layout: literal text, or a value.
enum class elem { lit, weekday, long_weekday, zero_day, under_day, month, year, short_year, hour, zero_minute,
                  zero_second, zone };

struct layout_elem {
    elem kind;
    std::string_view text = {};  // for lit
};

struct date_parser {
    std::string_view value;
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    int64 nsec = 0;
    int zone_offset = 0;  // seconds east of UTC

    static std::string_view cutspace(std::string_view s) {
        while (!s.empty() && s[0] == ' ') s.remove_prefix(1);
        return s;
    }

    // skip: removes prefix from value, treating runs of spaces as equal.
    bool skip(std::string_view prefix) {
        while (!prefix.empty()) {
            if (prefix[0] == ' ') {
                if (!value.empty() && value[0] != ' ') return false;
                prefix = cutspace(prefix);
                value = cutspace(value);
                continue;
            }
            if (value.empty() || value[0] != prefix[0]) return false;
            prefix.remove_prefix(1);
            value.remove_prefix(1);
        }
        return true;
    }

    // lookup: the index of the name that value starts with, ignoring case.
    template <std::size_t N>
    int lookup(const std::string_view (&names)[N]) {
        for (std::size_t i = 0; i < N; i++) {
            if (value.size() >= names[i].size() && strings::EqualFold(value.substr(0, names[i].size()), names[i])) {
                value.remove_prefix(names[i].size());
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    static bool isdigit(std::string_view s, std::size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; }

    // getnum: a one or two digit number; exactly two if fixed.
    bool getnum(int& out, bool fixed) {
        if (!isdigit(value, 0)) return false;
        if (!isdigit(value, 1)) {
            if (fixed) return false;
            out = value[0] - '0';
            value.remove_prefix(1);
            return true;
        }
        out = (value[0] - '0') * 10 + (value[1] - '0');
        value.remove_prefix(2);
        return true;
    }

    // A signed hour offset after GMT or alone: its length, or 0.
    static std::size_t signed_offset(std::string_view v, int& hours) {
        if (v.empty() || (v[0] != '-' && v[0] != '+')) return 0;
        std::size_t i = 1;
        int64 x = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9' && x < 100) x = x * 10 + (v[i++] - '0');
        if (i == 1 || x > 23) return 0;
        hours = static_cast<int>(v[0] == '-' ? -x : x);
        return i;
    }

    // parseTimeZone: an abbreviation such as GMT, MST or GMT+3. Go uses the
    // local zone's offset for its abbreviation and 0 for others; go++ uses 0.
    bool zone() {
        if (value.size() < 3) return false;
        if (value.starts_with("ChST") || value.starts_with("MeST")) {
            value.remove_prefix(4);
            return true;
        }
        if (value.starts_with("GMT")) {
            int hours = 0;
            std::size_t n = signed_offset(value.substr(3), hours);
            zone_offset = hours * 3600;
            value.remove_prefix(3 + n);
            return true;
        }
        if (value[0] == '+' || value[0] == '-') {
            int hours = 0;
            std::size_t n = signed_offset(value, hours);
            if (n == 0) return false;
            zone_offset = hours * 3600;
            value.remove_prefix(n);
            return true;
        }
        std::size_t upper = 0;
        while (upper < 6 && upper < value.size() && value[upper] >= 'A' && value[upper] <= 'Z') upper++;
        std::size_t n = 0;
        switch (upper) {
        case 3: n = 3; break;
        case 4: if (value[3] == 'T' || value.starts_with("WITA")) n = 4; break;
        case 5: if (value[4] == 'T') n = 5; break;
        default: break;
        }
        if (n == 0) return false;
        value.remove_prefix(n);
        return true;
    }

    bool parse(std::span<const layout_elem> layout) {
        for (const layout_elem& e : layout) {
            switch (e.kind) {
            case elem::lit:
                if (!skip(e.text)) return false;
                break;
            case elem::weekday:
                // The day of the week is checked for syntax, not used.
                if (lookup(short_day_names) < 0) return false;
                break;
            case elem::long_weekday:
                if (lookup(long_day_names) < 0) return false;
                break;
            case elem::zero_day:
            case elem::under_day:
                if (e.kind == elem::under_day && !value.empty() && value[0] == ' ') value.remove_prefix(1);
                if (!getnum(day, e.kind == elem::zero_day)) return false;
                break;
            case elem::month: {
                int m = lookup(short_month_names);
                if (m < 0) return false;
                month = m + 1;
                break;
            }
            case elem::year:
                if (value.size() < 4 || !isdigit(value, 0) || !isdigit(value, 1) || !isdigit(value, 2) ||
                    !isdigit(value, 3)) {
                    return false;
                }
                year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 + (value[3] - '0');
                value.remove_prefix(4);
                break;
            case elem::short_year:
                if (!isdigit(value, 0) || !isdigit(value, 1)) return false;
                year = (value[0] - '0') * 10 + (value[1] - '0');
                year += year >= 69 ? 1900 : 2000;
                value.remove_prefix(2);
                break;
            case elem::hour:
                if (!getnum(hour, false) || hour > 23) return false;
                break;
            case elem::zero_minute:
                if (!getnum(min, true) || min > 59) return false;
                break;
            case elem::zero_second:
                if (!getnum(sec, true) || sec > 59) return false;
                // A fractional second the layout doesn't have is accepted.
                if (value.size() >= 2 && (value[0] == '.' || value[0] == ',') && isdigit(value, 1)) {
                    std::size_t n = 1;
                    int64 scale = 100000000;
                    while (isdigit(value, n)) {
                        if (n <= 9) {
                            nsec += (value[n] - '0') * scale;
                            scale /= 10;
                        }
                        n++;
                    }
                    value.remove_prefix(n);
                }
                break;
            case elem::zone:
                if (!zone()) return false;
                break;
            }
        }
        if (!value.empty()) return false;  // extra text
        // Validate the day of the month.
        static constexpr int days_before[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        int dim = days_before[month - 1] + (month == 2 && leap ? 1 : 0);
        return day >= 1 && day <= dim;
    }

    // Days since 1970-01-01 of the civil date (Howard Hinnant's algorithm).
    static int64 days_from_civil(int64 y, int m, int d) {
        y -= m <= 2;
        int64 era = (y >= 0 ? y : y - 399) / 400;
        int64 yoe = y - era * 400;
        int64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        int64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    time::Time result() const {
        int64 secs = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec - zone_offset;
        return time::Time::Unix(secs, nsec);
    }
};

// TimeFormat, time.RFC850 and time.ANSIC.
constexpr layout_elem time_format_layout[] = {
    {elem::weekday}, {elem::lit, ", "}, {elem::zero_day}, {elem::lit, " "}, {elem::month}, {elem::lit, " "},
    {elem::year}, {elem::lit, " "}, {elem::hour}, {elem::lit, ":"}, {elem::zero_minute}, {elem::lit, ":"},
    {elem::zero_second}, {elem::lit, " GMT"},
};
constexpr layout_elem rfc850_layout[] = {
    {elem::long_weekday}, {elem::lit, ", "}, {elem::zero_day}, {elem::lit, "-"}, {elem::month}, {elem::lit, "-"},
    {elem::short_year}, {elem::lit, " "}, {elem::hour}, {elem::lit, ":"}, {elem::zero_minute}, {elem::lit, ":"},
    {elem::zero_second}, {elem::lit, " "}, {elem::zone},
};
constexpr layout_elem ansic_layout[] = {
    {elem::weekday}, {elem::lit, " "}, {elem::month}, {elem::lit, " "}, {elem::under_day}, {elem::lit, " "},
    {elem::hour}, {elem::lit, ":"}, {elem::zero_minute}, {elem::lit, ":"}, {elem::zero_second}, {elem::lit, " "},
    {elem::year},
};

}  // namespace

std::pair<time::Time, gopp::error> ParseTime(std::string_view text) {
    struct {
        std::span<const layout_elem> elems;
        std::string_view name;
    } layouts[] = {
        {time_format_layout, TimeFormat},
        {rfc850_layout, "Monday, 02-Jan-06 15:04:05 MST"},
        {ansic_layout, "Mon Jan _2 15:04:05 2006"},
    };
    for (const auto& l : layouts) {
        date_parser p{.value = text};
        if (p.parse(l.elems)) return {p.result(), nullptr};
    }
    return {{}, fmt::Errorf("parsing time {} as {}: cannot parse", strconv::Quote(text),
                            strconv::Quote(layouts[2].name))};
}

}  // namespace gopp::http
