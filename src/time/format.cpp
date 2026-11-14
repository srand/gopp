// Formatting and parsing: Go 1.27.3's format.go and format_rfc3339.go.
#include "internal.hpp"

#include <gopp/errors.hpp>

#include <cstring>

namespace gopp::time {

namespace {

// The std chunks of a layout (Go's stdLongMonth, ...).
enum : int {
    stdNeedDate = 1 << 8,    // need month, day, year
    stdNeedYday = 1 << 9,    // need yday
    stdNeedClock = 1 << 10,  // need hour, minute, second
    stdArgShift = 16,        // extra argument in high bits, above low stdArgShift
    stdSeparatorShift = 28,  // extra argument in high 4 bits for fractional second separators
    stdMask = (1 << stdArgShift) - 1,

    stdLongMonth = 1 + stdNeedDate,  // "January"
    stdMonth,                        // "Jan"
    stdNumMonth,                     // "1"
    stdZeroMonth,                    // "01"
    stdLongWeekDay,                  // "Monday"
    stdWeekDay,                      // "Mon"
    stdDay,                          // "2"
    stdUnderDay,                     // "_2"
    stdZeroDay,                      // "02"
    stdUnderYearDay = 10 + stdNeedYday,  // "__2"
    stdZeroYearDay,                      // "002"
    stdHour = 12 + stdNeedClock,         // "15"
    stdHour12,                           // "3"
    stdZeroHour12,                       // "03"
    stdMinute,                           // "4"
    stdZeroMinute,                       // "04"
    stdSecond,                           // "5"
    stdZeroSecond,                       // "05"
    stdLongYear = 19 + stdNeedDate,      // "2006"
    stdYear,                             // "06"
    stdPM = 21 + stdNeedClock,           // "PM"
    stdpm,                               // "pm"
    stdTZ = 23,                          // "MST"
    stdISO8601TZ,                        // "Z0700"  // prints Z for UTC
    stdISO8601SecondsTZ,                 // "Z070000"
    stdISO8601ShortTZ,                   // "Z07"
    stdISO8601ColonTZ,                   // "Z07:00" // prints Z for UTC
    stdISO8601ColonSecondsTZ,            // "Z07:00:00"
    stdNumTZ,                            // "-0700"  // always numeric
    stdNumSecondsTz,                     // "-070000"
    stdNumShortTZ,                       // "-07"    // always numeric
    stdNumColonTZ,                       // "-07:00" // always numeric
    stdNumColonSecondsTZ,                // "-07:00:00"
    stdFracSecond0,                      // ".0", ".00", ... , trailing zeros included
    stdFracSecond9,                      // ".9", ".99", ..., trailing zeros omitted
};

constexpr int std0x[] = {stdZeroMonth, stdZeroDay, stdZeroHour12, stdZeroMinute, stdZeroSecond, stdYear};

bool startsWithLowerCase(std::string_view s) { return !s.empty() && 'a' <= s[0] && s[0] <= 'z'; }

bool isDigit(std::string_view s, std::size_t i) { return i < s.size() && '0' <= s[i] && s[i] <= '9'; }

int stdFracSecond(int code, int n, int c) {
    // Use 0xfff to make the failure case even more absurd.
    if (c == '.') return code | ((n & 0xfff) << stdArgShift);
    return code | ((n & 0xfff) << stdArgShift) | 1 << stdSeparatorShift;
}

int digitsLen(int std) { return (std >> stdArgShift) & 0xfff; }

char separator(int std) { return (std >> stdSeparatorShift) == 0 ? '.' : ','; }

struct chunk {
    std::string_view prefix;
    int std = 0;
    std::string_view suffix;
};

// Go's nextStdChunk: the prefix before the first std chunk of layout, the
// chunk, and the rest.
chunk nextStdChunk(std::string_view layout) {
    auto has = [&](std::size_t i, std::string_view s) { return layout.substr(i, s.size()) == s; };
    for (std::size_t i = 0; i < layout.size(); i++) {
        switch (int c = layout[i]) {
        case 'J':  // January, Jan
            if (has(i, "Jan")) {
                if (has(i, "January")) return {layout.substr(0, i), stdLongMonth, layout.substr(i + 7)};
                if (!startsWithLowerCase(layout.substr(i + 3))) return {layout.substr(0, i), stdMonth, layout.substr(i + 3)};
            }
            break;
        case 'M':  // Monday, Mon, MST
            if (layout.size() >= i + 3) {
                if (has(i, "Mon")) {
                    if (has(i, "Monday")) return {layout.substr(0, i), stdLongWeekDay, layout.substr(i + 6)};
                    if (!startsWithLowerCase(layout.substr(i + 3))) {
                        return {layout.substr(0, i), stdWeekDay, layout.substr(i + 3)};
                    }
                }
                if (has(i, "MST")) return {layout.substr(0, i), stdTZ, layout.substr(i + 3)};
            }
            break;
        case '0':  // 01, 02, 03, 04, 05, 06, 002
            if (layout.size() >= i + 2 && '1' <= layout[i + 1] && layout[i + 1] <= '6') {
                return {layout.substr(0, i), std0x[layout[i + 1] - '1'], layout.substr(i + 2)};
            }
            if (layout.size() >= i + 3 && layout[i + 1] == '0' && layout[i + 2] == '2') {
                return {layout.substr(0, i), stdZeroYearDay, layout.substr(i + 3)};
            }
            break;
        case '1':  // 15, 1
            if (layout.size() >= i + 2 && layout[i + 1] == '5') return {layout.substr(0, i), stdHour, layout.substr(i + 2)};
            return {layout.substr(0, i), stdNumMonth, layout.substr(i + 1)};
        case '2':  // 2006, 2
            if (has(i, "2006")) return {layout.substr(0, i), stdLongYear, layout.substr(i + 4)};
            return {layout.substr(0, i), stdDay, layout.substr(i + 1)};
        case '_':  // _2, _2006, __2
            if (layout.size() >= i + 2 && layout[i + 1] == '2') {
                // _2006 is really a literal _, followed by stdLongYear
                if (has(i + 1, "2006")) return {layout.substr(0, i + 1), stdLongYear, layout.substr(i + 5)};
                return {layout.substr(0, i), stdUnderDay, layout.substr(i + 2)};
            }
            if (layout.size() >= i + 3 && layout[i + 1] == '_' && layout[i + 2] == '2') {
                return {layout.substr(0, i), stdUnderYearDay, layout.substr(i + 3)};
            }
            break;
        case '3':
            return {layout.substr(0, i), stdHour12, layout.substr(i + 1)};
        case '4':
            return {layout.substr(0, i), stdMinute, layout.substr(i + 1)};
        case '5':
            return {layout.substr(0, i), stdSecond, layout.substr(i + 1)};
        case 'P':  // PM
            if (layout.size() >= i + 2 && layout[i + 1] == 'M') return {layout.substr(0, i), stdPM, layout.substr(i + 2)};
            break;
        case 'p':  // pm
            if (layout.size() >= i + 2 && layout[i + 1] == 'm') return {layout.substr(0, i), stdpm, layout.substr(i + 2)};
            break;
        case '-':  // -070000, -07:00:00, -0700, -07:00, -07
            if (has(i, "-070000")) return {layout.substr(0, i), stdNumSecondsTz, layout.substr(i + 7)};
            if (has(i, "-07:00:00")) return {layout.substr(0, i), stdNumColonSecondsTZ, layout.substr(i + 9)};
            if (has(i, "-0700")) return {layout.substr(0, i), stdNumTZ, layout.substr(i + 5)};
            if (has(i, "-07:00")) return {layout.substr(0, i), stdNumColonTZ, layout.substr(i + 6)};
            if (has(i, "-07")) return {layout.substr(0, i), stdNumShortTZ, layout.substr(i + 3)};
            break;
        case 'Z':  // Z070000, Z07:00:00, Z0700, Z07:00,
            if (has(i, "Z070000")) return {layout.substr(0, i), stdISO8601SecondsTZ, layout.substr(i + 7)};
            if (has(i, "Z07:00:00")) return {layout.substr(0, i), stdISO8601ColonSecondsTZ, layout.substr(i + 9)};
            if (has(i, "Z0700")) return {layout.substr(0, i), stdISO8601TZ, layout.substr(i + 5)};
            if (has(i, "Z07:00")) return {layout.substr(0, i), stdISO8601ColonTZ, layout.substr(i + 6)};
            if (has(i, "Z07")) return {layout.substr(0, i), stdISO8601ShortTZ, layout.substr(i + 3)};
            break;
        case '.':
        case ',':  // ,000, or .000, or ,999, or .999 - repeated digits for fractional seconds.
            if (i + 1 < layout.size() && (layout[i + 1] == '0' || layout[i + 1] == '9')) {
                char ch = layout[i + 1];
                std::size_t j = i + 1;
                while (j < layout.size() && layout[j] == ch) j++;
                // String of digits must end here - only fractional second
                // if all digits.
                if (!isDigit(layout, j)) {
                    int code = layout[i + 1] == '9' ? stdFracSecond9 : stdFracSecond0;
                    int std = stdFracSecond(code, static_cast<int>(j - (i + 1)), c);
                    return {layout.substr(0, i), std, layout.substr(j)};
                }
            }
            break;
        }
    }
    return {layout, 0, {}};
}

// Go's match: s1 and s2 equal ignoring ASCII case.
bool match(std::string_view s1, std::string_view s2) {
    for (std::size_t i = 0; i < s1.size(); i++) {
        unsigned char c1 = s1[i], c2 = s2[i];
        if (c1 != c2) {
            // Switch to lower-case; 'a'-'A' is known to be a single bit.
            c1 |= 'a' - 'A';
            c2 |= 'a' - 'A';
            if (c1 != c2 || c1 < 'a' || c1 > 'z') return false;
        }
    }
    return true;
}

const error errBad = errors::New("bad value for field");  // placeholder not passed to user

// Go's lookup: the index of the name in tab that val starts with.
template <std::size_t N>
std::pair<int, error> lookup(const char* const (&tab)[N], std::string_view& val) {
    for (std::size_t i = 0; i < N; i++) {
        std::string_view v = tab[i];
        if (val.size() >= v.size() && match(val.substr(0, v.size()), v)) {
            val.remove_prefix(v.size());
            return {static_cast<int>(i), nullptr};
        }
    }
    return {-1, errBad};
}

void appendNano(std::string& b, int nanosec, int std) {
    bool trim = (std & stdMask) == stdFracSecond9;
    int n = digitsLen(std);
    if (trim && (n == 0 || nanosec == 0)) return;
    char dot = separator(std);
    b += dot;
    appendInt(b, nanosec, 9);
    if (n < 9) b.resize(b.size() - 9 + n);
    if (trim) {
        while (!b.empty() && b.back() == '0') b.pop_back();
        if (!b.empty() && b.back() == dot) b.pop_back();
    }
}

}  // namespace

void appendInt(std::string& b, int64 x, int width) {
    uint64 u = static_cast<uint64>(x);
    if (x < 0) {
        b += '-';
        u = static_cast<uint64>(-x);
    }
    char buf[24];
    int w = sizeof(buf);
    do {
        buf[--w] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u > 0);
    for (int pad = width - (static_cast<int>(sizeof(buf)) - w); pad > 0; pad--) b += '0';
    b.append(buf + w, sizeof(buf) - w);
}

std::string quote(std::string_view s) {
    static constexpr char lowerhex[] = "0123456789abcdef";
    std::string buf = "\"";
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c >= 0x80 || c < ' ') {
            // Go ranges over runes: write every byte of the rune (or the
            // one invalid byte) as \x escapes.
            std::size_t width = 1;
            if (c >= 0xC0) {
                std::size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
                std::size_t k = 1;
                while (k < want && i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80) k++;
                if (k == want) width = want;
            }
            for (std::size_t j = 0; j < width; j++) {
                unsigned char b = s[i + j];
                buf += "\\x";
                buf += lowerhex[b >> 4];
                buf += lowerhex[b & 0xF];
            }
            i += width;
        } else {
            if (c == '"' || c == '\\') buf += '\\';
            buf += static_cast<char>(c);
            i++;
        }
    }
    buf += '"';
    return buf;
}

// ---------------------------------------------------------------------------
// Format

string Time::String() const {
    std::string b;
    internal::appendFormat(*this, b, "2006-01-02 15:04:05.999999999 -0700 MST");
    // Format monotonic clock reading as m=±ddd.nnnnnnnnn.
    if (hasMono_) {
        uint64 m2 = static_cast<uint64>(mono_);
        char sign = '+';
        if (mono_ < 0) {
            sign = '-';
            m2 = -m2;
        }
        uint64 m1 = m2 / 1000000000;
        m2 %= 1000000000;
        uint64 m0 = m1 / 1000000000;
        m1 %= 1000000000;
        b += " m=";
        b += sign;
        int wid = 0;
        if (m0 != 0) {
            appendInt(b, static_cast<int64>(m0), 0);
            wid = 9;
        }
        appendInt(b, static_cast<int64>(m1), wid);
        b += '.';
        appendInt(b, static_cast<int64>(m2), 9);
    }
    return string(b);
}

string Time::GoString() const {
    absSeconds abs = internal::absSec(*this);
    auto [year, month, day] = absDate(abs / secondsPerDay);
    auto [hour, minute, second] = absClock(abs);
    std::string b = "time.Date(";
    appendInt(b, year, 0);
    if (January <= month && month <= December) {
        b += ", time.";
        b += longMonthNames[month.value - 1];
    } else {
        // It's difficult to construct a time.Time with a date outside the
        // standard range but we might as well try to handle the case.
        appendInt(b, month.value, 0);
    }
    b += ", ";
    appendInt(b, day, 0);
    b += ", ";
    appendInt(b, hour, 0);
    b += ", ";
    appendInt(b, minute, 0);
    b += ", ";
    appendInt(b, second, 0);
    b += ", ";
    appendInt(b, Nanosecond(), 0);
    b += ", ";
    const time::Location* loc = Location();
    if (loc == time::UTC || !loc) {
        b += "time.UTC";
    } else if (loc == time::Local) {
        b += "time.Local";
    } else {
        // there are several options for how we could display this, none
        // of which are great.
        b += "time.Location(";
        b += quote(loc->name.view());
        b += ')';
    }
    b += ')';
    return string(b);
}

string Time::Format(std::string_view layout) const {
    std::string b;
    b.reserve(layout.size() + 10);
    if (layout == RFC3339) internal::appendFormatRFC3339(*this, b, false);
    else if (layout == RFC3339Nano) internal::appendFormatRFC3339(*this, b, true);
    else internal::appendFormat(*this, b, layout);
    return string(b);
}

slice<byte> Time::AppendFormat(slice<byte> b, std::string_view layout) const {
    string s = Format(layout);
    return append_span(std::move(b), std::span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size()));
}

void Time::internal::appendFormat(const Time& t, std::string& b, std::string_view layout) {
    auto [name, offset, abs] = locabs(t);
    absDays days = abs / secondsPerDay;
    int year = -1, day = 0, yday = -1, hour = -1, min = 0, sec = 0;
    time::Month month;
    // Each iteration generates one std value.
    while (!layout.empty()) {
        auto [prefix, std, suffix] = nextStdChunk(layout);
        if (!prefix.empty()) b += prefix;
        if (std == 0) break;
        layout = suffix;

        // Compute year, month, day if needed.
        if (year < 0 && (std & stdNeedDate)) std::tie(year, month, day) = absDate(days);
        if (yday < 0 && (std & stdNeedYday)) yday = absYearYday(days).second;
        // Compute hour, minute, second if needed.
        if (hour < 0 && (std & stdNeedClock)) std::tie(hour, min, sec) = absClock(abs);

        switch (std & stdMask) {
        case stdYear: {
            int y = year < 0 ? -year : year;
            appendInt(b, y % 100, 2);
            break;
        }
        case stdLongYear: appendInt(b, year, 4); break;
        case stdMonth: b += month.String().view().substr(0, 3); break;
        case stdLongMonth: b += month.String().view(); break;
        case stdNumMonth: appendInt(b, month.value, 0); break;
        case stdZeroMonth: appendInt(b, month.value, 2); break;
        case stdWeekDay: b += absWeekday(days).String().view().substr(0, 3); break;
        case stdLongWeekDay: b += absWeekday(days).String().view(); break;
        case stdDay: appendInt(b, day, 0); break;
        case stdUnderDay:
            if (day < 10) b += ' ';
            appendInt(b, day, 0);
            break;
        case stdZeroDay: appendInt(b, day, 2); break;
        case stdUnderYearDay:
            if (yday < 100) {
                b += ' ';
                if (yday < 10) b += ' ';
            }
            appendInt(b, yday, 0);
            break;
        case stdZeroYearDay: appendInt(b, yday, 3); break;
        case stdHour: appendInt(b, hour, 2); break;
        case stdHour12: {
            // Noon is 12PM, midnight is 12AM.
            int hr = hour % 12;
            if (hr == 0) hr = 12;
            appendInt(b, hr, 0);
            break;
        }
        case stdZeroHour12: {
            int hr = hour % 12;
            if (hr == 0) hr = 12;
            appendInt(b, hr, 2);
            break;
        }
        case stdMinute: appendInt(b, min, 0); break;
        case stdZeroMinute: appendInt(b, min, 2); break;
        case stdSecond: appendInt(b, sec, 0); break;
        case stdZeroSecond: appendInt(b, sec, 2); break;
        case stdPM: b += hour >= 12 ? "PM" : "AM"; break;
        case stdpm: b += hour >= 12 ? "pm" : "am"; break;
        case stdISO8601TZ:
        case stdISO8601ColonTZ:
        case stdISO8601SecondsTZ:
        case stdISO8601ShortTZ:
        case stdISO8601ColonSecondsTZ:
        case stdNumTZ:
        case stdNumColonTZ:
        case stdNumSecondsTz:
        case stdNumShortTZ:
        case stdNumColonSecondsTZ: {
            // Ugly special case. We cheat and take the "Z" variants to mean
            // "the time zone as formatted for ISO 8601".
            if (offset == 0 && (std == stdISO8601TZ || std == stdISO8601ColonTZ || std == stdISO8601SecondsTZ ||
                                std == stdISO8601ShortTZ || std == stdISO8601ColonSecondsTZ)) {
                b += 'Z';
                break;
            }
            int zone = offset / 60;  // convert to minutes
            int absoffset = offset;
            if (zone < 0) {
                b += '-';
                zone = -zone;
                absoffset = -absoffset;
            } else {
                b += '+';
            }
            appendInt(b, zone / 60, 2);
            if (std == stdISO8601ColonTZ || std == stdNumColonTZ || std == stdISO8601ColonSecondsTZ ||
                std == stdNumColonSecondsTZ) {
                b += ':';
            }
            if (std != stdNumShortTZ && std != stdISO8601ShortTZ) appendInt(b, zone % 60, 2);
            // append seconds if appropriate
            if (std == stdISO8601SecondsTZ || std == stdNumSecondsTz || std == stdNumColonSecondsTZ ||
                std == stdISO8601ColonSecondsTZ) {
                if (std == stdNumColonSecondsTZ || std == stdISO8601ColonSecondsTZ) b += ':';
                appendInt(b, absoffset % 60, 2);
            }
            break;
        }
        case stdTZ: {
            // Ugly special case. We cheat and take the "Z" variants to mean
            // "the time zone as formatted for ISO 8601".
            if (!name.empty()) {
                b += name.view();
                break;
            }
            // No time zone known for this time, but we must print one. Use
            // the -0700 format.
            int zone = offset / 60;  // convert to minutes
            if (zone < 0) {
                b += '-';
                zone = -zone;
            } else {
                b += '+';
            }
            appendInt(b, zone / 60, 2);
            appendInt(b, zone % 60, 2);
            break;
        }
        case stdFracSecond0:
        case stdFracSecond9: appendNano(b, t.Nanosecond(), std); break;
        }
    }
}

void Time::internal::appendFormatRFC3339(const Time& t, std::string& b, bool nanos) {
    auto [name, offset, abs] = locabs(t);
    auto [year, month, day] = absDate(abs / secondsPerDay);
    appendInt(b, year, 4);
    b += '-';
    appendInt(b, month.value, 2);
    b += '-';
    appendInt(b, day, 2);
    b += 'T';
    auto [hour, min, sec] = absClock(abs);
    appendInt(b, hour, 2);
    b += ':';
    appendInt(b, min, 2);
    b += ':';
    appendInt(b, sec, 2);
    if (nanos) appendNano(b, t.Nanosecond(), stdFracSecond(stdFracSecond9, 9, '.'));
    if (offset == 0) {
        b += 'Z';
        return;
    }
    int zone = offset / 60;  // convert to minutes
    if (zone < 0) {
        b += '-';
        zone = -zone;
    } else {
        b += '+';
    }
    appendInt(b, zone / 60, 2);
    b += ':';
    appendInt(b, zone % 60, 2);
}

error Time::internal::appendStrictRFC3339(const Time& t, std::string& b) {
    std::size_t n0 = b.size();
    appendFormatRFC3339(t, b, true);
    auto num2 = [](std::string_view s) { return 10 * (s[0] - '0') + (s[1] - '0'); };
    if (b[n0 + 4] != '-') return errors::New("year outside of range [0,9999]");  // year must be exactly 4 digits wide
    if (b.back() != 'Z') {
        char c = b[b.size() - 6];
        if (('0' <= c && c <= '9') || num2(std::string_view(b).substr(b.size() - 5)) >= 24) {
            return errors::New("timezone hour outside of range [0,23]");
        }
    }
    return nullptr;
}

string Time::FormatHTTP() const { return UTC().Format("Mon, 02 Jan 2006 15:04:05 GMT"); }

string Time::FormatRFC3339() const { return UTC().Format(RFC3339Nano); }

// ---------------------------------------------------------------------------
// Marshaling

namespace {

slice<byte> to_bytes(std::string_view s) {
    return append_span(slice<byte>{}, std::span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size()));
}

std::string_view as_view(std::span<const byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

}  // namespace

std::pair<slice<byte>, error> Time::MarshalJSON() const {
    std::string b = "\"";
    error err = internal::appendStrictRFC3339(*this, b);
    b += '"';
    // Changed to an error in Go 1.20: see https://go.dev/issue/4556.
    if (err) return {slice<byte>{}, errors::New("Time.MarshalJSON: " + err.Error())};
    return {to_bytes(b), nullptr};
}

error Time::UnmarshalJSON(std::span<const byte> data) {
    std::string_view s = as_view(data);
    if (s == "null") return nullptr;
    // TODO(https://go.dev/issue/47353): Properly unescape a JSON string.
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') {
        return errors::New("Time.UnmarshalJSON: input is not a JSON string");
    }
    s = s.substr(1, s.size() - 2);
    auto [t, err] = parseStrictRFC3339(s);
    *this = t;
    return err;
}

std::pair<slice<byte>, error> Time::AppendText(slice<byte> b) const {
    std::string s;
    if (error err = internal::appendStrictRFC3339(*this, s)) return {slice<byte>{}, errors::New("Time.AppendText: " + err.Error())};
    return {append_span(std::move(b), std::span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size())), nullptr};
}

std::pair<slice<byte>, error> Time::MarshalText() const {
    std::string s;
    if (error err = internal::appendStrictRFC3339(*this, s)) return {slice<byte>{}, errors::New("Time.MarshalText: " + err.Error())};
    return {to_bytes(s), nullptr};
}

error Time::UnmarshalText(std::span<const byte> data) {
    auto [t, err] = parseStrictRFC3339(as_view(data));
    *this = t;
    return err;
}

// ---------------------------------------------------------------------------
// Parse

string ParseError::Error() const {
    if (Message.empty()) {
        return string("parsing time " + quote(Value.view()) + " as " + quote(Layout.view()) + ": cannot parse " +
                      quote(ValueElem.view()) + " as " + quote(LayoutElem.view()));
    }
    return string("parsing time " + quote(Value.view())) + Message;
}

namespace {

error newParseError(std::string_view layout, std::string_view value, std::string_view layoutElem,
                    std::string_view valueElem, std::string_view message) {
    return make_ptr<ParseError>(string(layout), string(value), string(layoutElem), string(valueElem), string(message));
}

// Go's getnum: one or two digits (two if fixed).
std::pair<int, error> getnum(std::string_view& s, bool fixed) {
    if (!isDigit(s, 0)) return {0, errBad};
    if (!isDigit(s, 1)) {
        if (fixed) return {0, errBad};
        int n = s[0] - '0';
        s.remove_prefix(1);
        return {n, nullptr};
    }
    int n = (s[0] - '0') * 10 + (s[1] - '0');
    s.remove_prefix(2);
    return {n, nullptr};
}

// Go's getnum3: one to three digits (three if fixed).
std::pair<int, error> getnum3(std::string_view& s, bool fixed) {
    int n = 0;
    std::size_t i = 0;
    for (; i < 3 && isDigit(s, i); i++) n = n * 10 + (s[i] - '0');
    if (i == 0 || (fixed && i != 3)) return {0, errBad};
    s.remove_prefix(i);
    return {n, nullptr};
}

std::string_view cutspace(std::string_view s) {
    while (!s.empty() && s[0] == ' ') s.remove_prefix(1);
    return s;
}

// Go's skip: value without prefix; spaces in prefix match any number of
// spaces in value.
std::pair<std::string_view, error> skip(std::string_view value, std::string_view prefix) {
    while (!prefix.empty()) {
        if (prefix[0] == ' ') {
            if (!value.empty() && value[0] != ' ') return {value, errBad};
            prefix = cutspace(prefix);
            value = cutspace(value);
            continue;
        }
        if (value.empty() || value[0] != prefix[0]) return {value, errBad};
        prefix.remove_prefix(1);
        value.remove_prefix(1);
    }
    return {value, nullptr};
}

const error errLeadingInt = errors::New("time: bad [0-9]*");  // never printed

// Go's leadingInt: the leading digits of s.
std::pair<uint64, error> leadingInt(std::string_view& s) {
    uint64 x = 0;
    std::size_t i = 0;
    for (; i < s.size(); i++) {
        char c = s[i];
        if (c < '0' || c > '9') break;
        if (x > (uint64(1) << 63) / 10) return {0, errLeadingInt};  // overflow
        x = x * 10 + static_cast<uint64>(c - '0');
        if (x > uint64(1) << 63) return {0, errLeadingInt};  // overflow
    }
    s.remove_prefix(i);
    return {x, nullptr};
}

const error errAtoi = errors::New("time: invalid number");

// Go's atoi.
std::pair<int, error> atoi(std::string_view s) {
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s.remove_prefix(1);
    }
    auto [q, err] = leadingInt(s);
    int x = static_cast<int>(q);
    if (err || !s.empty()) return {0, errAtoi};
    return {neg ? -x : x, nullptr};
}

bool commaOrPeriod(char b) { return b == '.' || b == ','; }

struct nanos_result {
    int ns = 0;
    std::string_view rangeErrString;
    error err;
};

nanos_result parseNanoseconds(std::string_view value, std::size_t nbytes) {
    nanos_result r;
    if (!commaOrPeriod(value[0])) {
        r.err = errBad;
        return r;
    }
    if (nbytes > 10) {
        value = value.substr(0, 10);
        nbytes = 10;
    }
    std::tie(r.ns, r.err) = atoi(value.substr(1, nbytes - 1));
    if (r.err) return r;
    if (r.ns < 0) {
        r.rangeErrString = "fractional second";
        return r;
    }
    // We need nanoseconds, which means scaling by the number of missing
    // digits in the format, maximum length 10.
    for (std::size_t i = 0; i < 10 - nbytes; i++) r.ns *= 10;
    return r;
}

// Go's parseSignedOffset: the length of a valid signed offset ("+3") at
// the start of value, or 0.
std::size_t parseSignedOffset(std::string_view value) {
    char sign = value[0];
    if (sign != '-' && sign != '+') return 0;
    std::string_view rem = value.substr(1);
    auto [x, err] = leadingInt(rem);
    // fail if nothing consumed by leadingInt
    if (err || value.substr(1) == rem) return 0;
    if (x > 23) return 0;
    return value.size() - rem.size();
}

// Go's parseGMT: the length of "GMT" and an offset.
std::size_t parseGMT(std::string_view value) {
    value = value.substr(3);
    if (value.empty()) return 3;
    return 3 + parseSignedOffset(value);
}

// Go's parseTimeZone: the length of a time zone abbreviation at the start
// of value.
std::pair<std::size_t, bool> parseTimeZone(std::string_view value) {
    if (value.size() < 3) return {0, false};
    // Special case 1: ChST and MeST.
    if (value.size() >= 4 && (value.substr(0, 4) == "ChST" || value.substr(0, 4) == "MeST")) return {4, true};
    // Special case 2: GMT may have an hour offset; treat it specially.
    if (value.substr(0, 3) == "GMT") return {parseGMT(value), true};
    // Special Case 3: Some time zones are not named, but have +/-00 format
    if (value[0] == '+' || value[0] == '-') {
        std::size_t length = parseSignedOffset(value);
        return {length, length > 0};  // parseSignedOffset returns 0 in case of bad input
    }
    // How many upper-case letters are there? Need at least three, at most
    // five.
    int nUpper = 0;
    for (; nUpper < 6; nUpper++) {
        if (static_cast<std::size_t>(nUpper) >= value.size()) break;
        char c = value[nUpper];
        if (c < 'A' || 'Z' < c) break;
    }
    switch (nUpper) {
    case 0:
    case 1:
    case 2:
    case 6: return {0, false};
    case 5:  // Must end in T to match.
        if (value[4] == 'T') return {5, true};
        break;
    case 4:
        // Must end in T, except one special case.
        if (value[3] == 'T' || value.substr(0, 4) == "WITA") return {4, true};
        break;
    case 3: return {3, true};
    }
    return {0, false};
}

std::pair<Time, error> parse(std::string_view layout, std::string_view value, const Location* defaultLocation,
                             const Location* local) {
    std::string_view alayout = layout, avalue = value;
    std::string_view rangeErrString;  // set if a value is out of range
    bool amSet = false;               // do we need to subtract 12 from the hour for midnight?
    bool pmSet = false;               // do we need to add 12 to the hour?

    // Time being constructed.
    int year = 0, month = -1, day = -1, yday = -1, hour = 0, min = 0, sec = 0, nsec = 0;
    const Location* z = nullptr;
    int zoneOffset = -1;
    std::string_view zoneName;

    // Each iteration processes one std value.
    for (;;) {
        error err;
        auto [prefix, std, suffix] = nextStdChunk(layout);
        std::string_view stdstr = layout.substr(prefix.size(), layout.size() - prefix.size() - suffix.size());
        std::tie(value, err) = skip(value, prefix);
        if (err) return {Time{}, newParseError(alayout, avalue, prefix, value, "")};
        if (std == 0) {
            if (!value.empty()) {
                return {Time{}, newParseError(alayout, avalue, "", value, ": extra text: " + quote(value))};
            }
            break;
        }
        layout = suffix;
        std::string_view p;
        std::string_view hold = value;
        switch (std & stdMask) {
        case stdYear:
            if (value.size() < 2) {
                err = errBad;
                break;
            }
            p = value.substr(0, 2);
            value.remove_prefix(2);
            std::tie(year, err) = atoi(p);
            if (err) break;
            if (year >= 69) year += 1900;  // Unix time starts Dec 31 1969 in some time zones
            else year += 2000;
            break;
        case stdLongYear:
            if (value.size() < 4 || !isDigit(value, 0)) {
                err = errBad;
                break;
            }
            p = value.substr(0, 4);
            value.remove_prefix(4);
            std::tie(year, err) = atoi(p);
            break;
        case stdMonth:
            std::tie(month, err) = lookup(shortMonthNames, value);
            month++;
            break;
        case stdLongMonth:
            std::tie(month, err) = lookup(longMonthNames, value);
            month++;
            break;
        case stdNumMonth:
        case stdZeroMonth:
            std::tie(month, err) = getnum(value, std == stdZeroMonth);
            if (!err && (month <= 0 || 12 < month)) rangeErrString = "month";
            break;
        case stdWeekDay:
            // Ignore weekday except for error checking.
            err = lookup(shortDayNames, value).second;
            break;
        case stdLongWeekDay:
            err = lookup(longDayNames, value).second;
            break;
        case stdDay:
        case stdUnderDay:
        case stdZeroDay:
            if (std == stdUnderDay && !value.empty() && value[0] == ' ') value.remove_prefix(1);
            std::tie(day, err) = getnum(value, std == stdZeroDay);
            // Note that we allow any one- or two-digit day here. The month,
            // day, year combination is validated after we've completed
            // parsing.
            break;
        case stdUnderYearDay:
        case stdZeroYearDay:
            for (int i = 0; i < 2; i++) {
                if (std == stdUnderYearDay && !value.empty() && value[0] == ' ') value.remove_prefix(1);
            }
            std::tie(yday, err) = getnum3(value, std == stdZeroYearDay);
            // Note that we allow any one-, two-, or three-digit year-day
            // here. The year-day, year combination is validated after we've
            // completed parsing.
            break;
        case stdHour:
            std::tie(hour, err) = getnum(value, false);
            if (hour < 0 || 24 <= hour) rangeErrString = "hour";
            break;
        case stdHour12:
        case stdZeroHour12:
            std::tie(hour, err) = getnum(value, std == stdZeroHour12);
            if (hour < 0 || 12 < hour) rangeErrString = "hour";
            break;
        case stdMinute:
        case stdZeroMinute:
            std::tie(min, err) = getnum(value, std == stdZeroMinute);
            if (min < 0 || 60 <= min) rangeErrString = "minute";
            break;
        case stdSecond:
        case stdZeroSecond: {
            std::tie(sec, err) = getnum(value, std == stdZeroSecond);
            if (err) break;
            if (sec < 0 || 60 <= sec) {
                rangeErrString = "second";
                break;
            }
            // Special case: do we have a fractional second but no
            // fractional second in the format?
            if (value.size() >= 2 && commaOrPeriod(value[0]) && isDigit(value, 1)) {
                int nstd = nextStdChunk(layout).std & stdMask;
                if (nstd == stdFracSecond0 || nstd == stdFracSecond9) {
                    // Fractional second in the layout; proceed normally
                    break;
                }
                // No fractional second in the layout but we have one in the
                // input.
                std::size_t n = 2;
                for (; n < value.size() && isDigit(value, n); n++) {
                }
                auto r = parseNanoseconds(value, n);
                nsec = r.ns;
                rangeErrString = r.rangeErrString;
                err = r.err;
                value.remove_prefix(n);
            }
            break;
        }
        case stdPM:
            if (value.size() < 2) {
                err = errBad;
                break;
            }
            p = value.substr(0, 2);
            value.remove_prefix(2);
            if (p == "PM") pmSet = true;
            else if (p == "AM") amSet = true;
            else err = errBad;
            break;
        case stdpm:
            if (value.size() < 2) {
                err = errBad;
                break;
            }
            p = value.substr(0, 2);
            value.remove_prefix(2);
            if (p == "pm") pmSet = true;
            else if (p == "am") amSet = true;
            else err = errBad;
            break;
        case stdISO8601TZ:
        case stdISO8601ShortTZ:
        case stdISO8601ColonTZ:
        case stdISO8601SecondsTZ:
        case stdISO8601ColonSecondsTZ:
            if (!value.empty() && value[0] == 'Z') {
                value.remove_prefix(1);
                z = UTC;
                break;
            }
            [[fallthrough]];
        case stdNumTZ:
        case stdNumShortTZ:
        case stdNumColonTZ:
        case stdNumSecondsTz:
        case stdNumColonSecondsTZ: {
            std::string_view sign, hr, mn, seconds;
            if (std == stdISO8601ColonTZ || std == stdNumColonTZ) {
                if (value.size() < 6 || value[3] != ':') {
                    err = errBad;
                    break;
                }
                sign = value.substr(0, 1), hr = value.substr(1, 2), mn = value.substr(4, 2), seconds = "00";
                value.remove_prefix(6);
            } else if (std == stdNumShortTZ || std == stdISO8601ShortTZ) {
                if (value.size() < 3) {
                    err = errBad;
                    break;
                }
                sign = value.substr(0, 1), hr = value.substr(1, 2), mn = "00", seconds = "00";
                value.remove_prefix(3);
            } else if (std == stdISO8601ColonSecondsTZ || std == stdNumColonSecondsTZ) {
                if (value.size() < 9 || value[3] != ':' || value[6] != ':') {
                    err = errBad;
                    break;
                }
                sign = value.substr(0, 1), hr = value.substr(1, 2), mn = value.substr(4, 2), seconds = value.substr(7, 2);
                value.remove_prefix(9);
            } else if (std == stdISO8601SecondsTZ || std == stdNumSecondsTz) {
                if (value.size() < 7) {
                    err = errBad;
                    break;
                }
                sign = value.substr(0, 1), hr = value.substr(1, 2), mn = value.substr(3, 2), seconds = value.substr(5, 2);
                value.remove_prefix(7);
            } else {
                if (value.size() < 5) {
                    err = errBad;
                    break;
                }
                sign = value.substr(0, 1), hr = value.substr(1, 2), mn = value.substr(3, 2), seconds = "00";
                value.remove_prefix(5);
            }
            int h = 0, mm = 0, ss = 0;
            std::tie(h, err) = getnum(hr, true);
            if (!err) {
                std::tie(mm, err) = getnum(mn, true);
                if (!err) std::tie(ss, err) = getnum(seconds, true);
            }
            // The range test use > rather than >=, as some people do write
            // offsets of 24 hours or 60 minutes or 60 seconds.
            if (h > 24) rangeErrString = "time zone offset hour";
            if (mm > 60) rangeErrString = "time zone offset minute";
            if (ss > 60) rangeErrString = "time zone offset second";
            zoneOffset = (h * 60 + mm) * 60 + ss;  // offset is in seconds
            switch (sign[0]) {
            case '+': break;
            case '-': zoneOffset = -zoneOffset; break;
            default: err = errBad;
            }
            break;
        }
        case stdTZ: {
            // Does it look like a time zone?
            if (value.size() >= 3 && value.substr(0, 3) == "UTC") {
                z = UTC;
                value.remove_prefix(3);
                break;
            }
            auto [n, ok] = parseTimeZone(value);
            if (!ok) {
                err = errBad;
                break;
            }
            zoneName = value.substr(0, n);
            value.remove_prefix(n);
            break;
        }
        case stdFracSecond0: {
            // stdFracSecond0 requires the exact number of digits as
            // specified in the layout.
            std::size_t ndigit = 1 + digitsLen(std);
            if (value.size() < ndigit) {
                err = errBad;
                break;
            }
            auto r = parseNanoseconds(value, ndigit);
            nsec = r.ns;
            rangeErrString = r.rangeErrString;
            err = r.err;
            value.remove_prefix(ndigit);
            break;
        }
        case stdFracSecond9: {
            if (value.size() < 2 || !commaOrPeriod(value[0]) || value[1] < '0' || '9' < value[1]) {
                // Fractional second omitted.
                break;
            }
            // Take any number of digits, even more than asked for, because
            // it is what the stdSecond case would do.
            std::size_t i = 0;
            while (i + 1 < value.size() && '0' <= value[i + 1] && value[i + 1] <= '9') i++;
            auto r = parseNanoseconds(value, 1 + i);
            nsec = r.ns;
            rangeErrString = r.rangeErrString;
            err = r.err;
            value.remove_prefix(1 + i);
            break;
        }
        }
        if (!rangeErrString.empty()) {
            return {Time{}, newParseError(alayout, avalue, stdstr, value, ": " + std::string(rangeErrString) + " out of range")};
        }
        if (err) return {Time{}, newParseError(alayout, avalue, stdstr, hold, "")};
    }
    if (pmSet && hour < 12) hour += 12;
    else if (amSet && hour == 12) hour = 0;

    // Convert yday to day, month.
    if (yday >= 0) {
        int d = 0, m = 0;
        if (isLeap(year)) {
            if (yday == 31 + 29) {
                m = February.value;
                d = 29;
            } else if (yday > 31 + 29) {
                yday--;
            }
        }
        if (yday < 1 || yday > 365) return {Time{}, newParseError(alayout, avalue, "", value, ": day-of-year out of range")};
        if (m == 0) {
            m = (yday - 1) / 31 + 1;
            if (daysBefore(Month(m + 1)) < yday) m++;
            d = yday - daysBefore(Month(m));
        }
        // If month, day already seen, yday's m, d must match. That makes
        // exact the "day-of-year does not match" errors.
        if (month >= 0 && month != m) {
            return {Time{}, newParseError(alayout, avalue, "", value, ": day-of-year does not match month")};
        }
        month = m;
        if (day >= 0 && day != d) {
            return {Time{}, newParseError(alayout, avalue, "", value, ": day-of-year does not match day")};
        }
        day = d;
    } else {
        if (month < 0) month = January.value;
        if (day < 0) day = 1;
    }

    // Validate the day of the month.
    if (day < 1 || day > daysIn(Month(month), year)) {
        return {Time{}, newParseError(alayout, avalue, "", value, ": day out of range")};
    }

    if (z) return {Date(year, Month(month), day, hour, min, sec, nsec, z), nullptr};

    if (zoneOffset != -1) {
        Time t = Date(year, Month(month), day, hour, min, sec, nsec, UTC);
        Time::internal::addSec(t, -static_cast<int64>(zoneOffset));
        // Look for local zone with the given offset. If that zone was in
        // effect at the given time, use it.
        auto r = local->lookup(Time::internal::unixSec(t));
        if (r.offset == zoneOffset && (zoneName.empty() || r.name.view() == zoneName)) {
            Time::internal::setLoc(t, local);
            return {t, nullptr};
        }
        // Otherwise create fake zone to record offset.
        Time::internal::setLoc(t, FixedZone(zoneName, zoneOffset));
        return {t, nullptr};
    }

    if (!zoneName.empty()) {
        Time t = Date(year, Month(month), day, hour, min, sec, nsec, UTC);
        // Look for local zone with the given offset. If that zone was in
        // effect at the given time, use it.
        auto [offset, ok] = local->lookupName(zoneName, Time::internal::unixSec(t));
        if (ok) {
            Time::internal::addSec(t, -static_cast<int64>(offset));
            Time::internal::setLoc(t, local);
            return {t, nullptr};
        }
        // Otherwise, create fake zone with unknown offset.
        if (zoneName.size() > 3 && zoneName.substr(0, 3) == "GMT") {
            offset = atoi(zoneName.substr(3)).first;  // Guaranteed OK by parseGMT.
            offset *= 3600;
        }
        Time::internal::setLoc(t, FixedZone(zoneName, offset));
        return {t, nullptr};
    }

    // Otherwise, fall back to default.
    return {Date(year, Month(month), day, hour, min, sec, nsec, defaultLocation), nullptr};
}

// Go's parseRFC3339: the fast path of Parse for RFC 3339 values.
std::pair<Time, bool> parseRFC3339(std::string_view s, const Location* local) {
    bool ok = true;
    auto parseUint = [&](std::string_view s, int min, int max) {
        int x = 0;
        for (char c : s) {
            if (c < '0' || '9' < c) {
                ok = false;
                return min;
            }
            x = x * 10 + (c - '0');
        }
        if (x < min || max < x) {
            ok = false;
            return min;
        }
        return x;
    };

    // Parse the date and time.
    if (s.size() < std::string_view("2006-01-02T15:04:05").size()) return {Time{}, false};
    int year = parseUint(s.substr(0, 4), 0, 9999);                       // e.g., 2006
    int month = parseUint(s.substr(5, 2), 1, 12);                        // e.g., 01
    int day = parseUint(s.substr(8, 2), 1, daysIn(Month(month), year));  // e.g., 02
    int hour = parseUint(s.substr(11, 2), 0, 23);                        // e.g., 15
    int min = parseUint(s.substr(14, 2), 0, 59);                         // e.g., 04
    int sec = parseUint(s.substr(17, 2), 0, 59);                         // e.g., 05
    if (!ok || !(s[4] == '-' && s[7] == '-' && s[10] == 'T' && s[13] == ':' && s[16] == ':')) return {Time{}, false};
    s.remove_prefix(19);

    // Parse the fractional second.
    int nsec = 0;
    if (s.size() >= 2 && s[0] == '.' && isDigit(s, 1)) {
        std::size_t n = 2;
        for (; n < s.size() && isDigit(s, n); n++) {
        }
        nsec = parseNanoseconds(s, n).ns;
        s.remove_prefix(n);
    }

    // Parse the time zone.
    Time t = Date(year, Month(month), day, hour, min, sec, nsec, UTC);
    if (s.size() != 1 || s[0] != 'Z') {
        if (s.size() != std::string_view("-07:00").size()) return {Time{}, false};
        int hr = parseUint(s.substr(1, 2), 0, 23);  // e.g., 07
        int mm = parseUint(s.substr(4, 2), 0, 59);  // e.g., 00
        if (!ok || !((s[0] == '-' || s[0] == '+') && s[3] == ':')) return {Time{}, false};
        int zoneOffset = (hr * 60 + mm) * 60;
        if (s[0] == '-') zoneOffset *= -1;
        Time::internal::addSec(t, -static_cast<int64>(zoneOffset));

        // Use local zone with the given offset if possible.
        if (local->lookup(Time::internal::unixSec(t)).offset == zoneOffset) Time::internal::setLoc(t, local);
        else Time::internal::setLoc(t, FixedZone("", zoneOffset));
    }
    return {t, true};
}

}  // namespace

std::pair<Time, error> parseStrictRFC3339(std::string_view b) {
    auto [t, ok] = parseRFC3339(b, Local);
    if (!ok) {
        auto [t2, err] = Parse(RFC3339, b);
        if (err) return {Time{}, err};
        // The parse template syntax cannot correctly validate RFC 3339.
        // Explicitly check for cases that Parse is unable to validate for.
        // See https://go.dev/issue/54580. Go 1.27 accepts them again for
        // compatibility (the switch returns first), and so does go++.
        return {t2, nullptr};
    }
    return {t, nullptr};
}

std::pair<Time, error> Parse(std::string_view layout, std::string_view value) {
    // Optimize for RFC3339 as it accounts for over half of all
    // representations.
    if (layout == RFC3339 || layout == RFC3339Nano) {
        if (auto [t, ok] = parseRFC3339(value, Local); ok) return {t, nullptr};
    }
    return parse(layout, value, UTC, Local);
}

std::pair<Time, error> ParseInLocation(std::string_view layout, std::string_view value, const Location* loc) {
    // Optimize for RFC3339 as it accounts for over half of all
    // representations.
    if (layout == RFC3339 || layout == RFC3339Nano) {
        if (auto [t, ok] = parseRFC3339(value, loc); ok) return {t, nullptr};
    }
    return parse(layout, value, loc, loc);
}

namespace testing_hooks {

std::tuple<std::string, int, std::string> NextStdChunk(std::string_view layout) {
    auto c = nextStdChunk(layout);
    return {std::string(c.prefix), c.std, std::string(c.suffix)};
}

std::pair<int, bool> ParseTimeZone(std::string_view value) {
    auto [n, ok] = parseTimeZone(value);
    return {static_cast<int>(n), ok};
}

}  // namespace testing_hooks

}  // namespace gopp::time
