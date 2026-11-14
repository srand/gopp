// Locations: Go 1.27.3's zoneinfo.go, zoneinfo_read.go and
// zoneinfo_unix.go.
//
// Locations live as long as the program. The ones LoadLocation and
// LoadLocationFromTZData make are shared per name and data, the ones
// FixedZone makes per name and offset, so that loading a zone again doesn't
// grow memory. Zone files are read with plain system calls, as Go's are:
// nothing here parks a fiber, so first uses from many fibers at once can
// wait on a mutex safely.
#include "internal.hpp"

#include <gopp/errors.hpp>
#include <gopp/syscall.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <tuple>

namespace gopp::time {

namespace {

// Storage that is constant-initialized and never destroyed: fibers may
// still format times while the process exits.
template <typename T>
union neverDestroyed {
    T value;
    constexpr neverDestroyed() : value() {}
    ~neverDestroyed() {}
};

constinit neverDestroyed<Location> utc_storage;
constinit neverDestroyed<Location> local_storage;

Location& utcLoc() { return utc_storage.value; }
Location& localLoc() { return local_storage.value; }

// Go's localOnce, resettable for tests.
std::atomic<bool> local_done{false};
std::mutex& localMu() {
    static auto* m = new std::mutex;
    return *m;
}

void initLocal();

void ensureLocal() {
    if (local_done.load(std::memory_order_acquire)) return;
    std::lock_guard l(localMu());
    if (!local_done.load(std::memory_order_relaxed)) {
        initLocal();
        local_done.store(true, std::memory_order_release);
    }
}

}  // namespace

const Location* const UTC = &utc_storage.value;
const Location* Local = &local_storage.value;

namespace {
// utcLoc's name, set before main; until then String() still says "UTC".
const bool utc_named = [] {
    utcLoc().name = string::fromStatic("UTC");
    return true;
}();
}  // namespace

const Location* Location::get() const {
    if (this == &local_storage.value) ensureLocal();
    return this;
}

string Location::String() const {
    if (this == UTC) return string::fromStatic("UTC");
    return get()->name;
}

// ---------------------------------------------------------------------------
// Lookups

namespace {

std::tuple<std::string_view, std::string_view, bool> tzsetName(std::string_view s);
std::tuple<int, std::string_view, bool> tzsetOffset(std::string_view s);

struct rule {
    int kind = 0;  // ruleJulian, ruleDOY, ruleMonthWeekDay
    int day = 0;
    int week = 0;
    int mon = 0;
    int time = 0;  // transition time
};
constexpr int ruleJulian = 0, ruleDOY = 1, ruleMonthWeekDay = 2;

std::tuple<rule, std::string_view, bool> tzsetRule(std::string_view s);

struct tzset_result {
    std::string_view name;
    int offset = 0;
    int64 start = 0, end = 0;
    bool isDST = false, ok = false;
};

tzset_result tzset(std::string_view s, int64 lastTxSec, int64 sec);

}  // namespace

Location::lookup_result Location::lookup(int64 sec) const {
    const Location* l = get();
    lookup_result r;
    if (l->zones.empty()) {
        r.name = string::fromStatic("UTC");
        r.offset = 0;
        r.start = alpha;
        r.end = omega;
        r.isDST = false;
        return r;
    }
    if (l->hasCache && l->cacheStart <= sec && sec < l->cacheEnd) {
        r.name = l->cacheZone.name;
        r.offset = l->cacheZone.offset;
        r.start = l->cacheStart;
        r.end = l->cacheEnd;
        r.isDST = l->cacheZone.isDST;
        return r;
    }
    if (l->tx.empty() || sec < l->tx[0].when) {
        const zone& z = l->zones[l->lookupFirstZone()];
        r.name = z.name;
        r.offset = z.offset;
        r.start = alpha;
        r.end = l->tx.empty() ? omega : l->tx[0].when;
        r.isDST = z.isDST;
        return r;
    }
    // Binary search for entry with largest time <= sec.
    const auto& tx = l->tx;
    int64 end = omega;
    std::size_t lo = 0, hi = tx.size();
    while (hi - lo > 1) {
        std::size_t m = (lo + hi) / 2;
        int64 lim = tx[m].when;
        if (sec < lim) {
            end = lim;
            hi = m;
        } else {
            lo = m;
        }
    }
    const zone& z = l->zones[tx[lo].index];
    r.name = z.name;
    r.offset = z.offset;
    r.start = tx[lo].when;
    r.end = end;
    r.isDST = z.isDST;
    // If we're at the end of the known zone transitions, try the extend
    // string.
    if (lo == tx.size() - 1 && !l->extend.empty()) {
        auto e = tzset(l->extend, r.start, sec);
        if (e.ok) {
            // The name is a substring of extend, which lives as long as l.
            r.name = string::fromStatic(e.name);
            r.offset = e.offset;
            r.start = e.start;
            r.end = e.end;
            r.isDST = e.isDST;
        }
    }
    return r;
}

int Location::lookupFirstZone() const {
    // Case 1.
    if (!firstZoneUsed()) return 0;
    // Case 2.
    if (!tx.empty() && zones[tx[0].index].isDST) {
        for (int zi = static_cast<int>(tx[0].index) - 1; zi >= 0; zi--) {
            if (!zones[zi].isDST) return zi;
        }
    }
    // Case 3.
    for (std::size_t zi = 0; zi < zones.size(); zi++) {
        if (!zones[zi].isDST) return static_cast<int>(zi);
    }
    // Case 4.
    return 0;
}

bool Location::firstZoneUsed() const {
    for (const auto& t : tx) {
        if (t.index == 0) return true;
    }
    return false;
}

std::pair<int, bool> Location::lookupName(std::string_view name, int64 unix) const {
    const Location* l = get();
    // First try for a zone with the right name that was actually in effect
    // at the given time.
    for (const auto& z : l->zones) {
        if (z.name.view() == name) {
            auto r = l->lookup(unix - z.offset);
            if (r.name.view() == z.name.view()) return {r.offset, true};
        }
    }
    // Otherwise fall back to an ordinary name match.
    for (const auto& z : l->zones) {
        if (z.name.view() == name) return {z.offset, true};
    }
    return {0, false};
}

// ---------------------------------------------------------------------------
// POSIX TZ strings (Go's tzset and friends)

namespace {

std::tuple<int, std::string_view, bool> tzsetNum(std::string_view s, int min, int max) {
    if (s.empty()) return {0, {}, false};
    int num = 0;
    for (std::size_t i = 0; i < s.size(); i++) {
        char r = s[i];
        if (r < '0' || r > '9') {
            if (i == 0 || num < min) return {0, {}, false};
            return {num, s.substr(i), true};
        }
        num = num * 10 + (r - '0');
        if (num > max) return {0, {}, false};
    }
    if (num < min) return {0, {}, false};
    return {num, {}, true};
}

std::tuple<std::string_view, std::string_view, bool> tzsetName(std::string_view s) {
    if (s.empty()) return {{}, {}, false};
    if (s[0] != '<') {
        for (std::size_t i = 0; i < s.size(); i++) {
            switch (s[i]) {
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            case ',': case '-': case '+':
                if (i < 3) return {{}, {}, false};
                return {s.substr(0, i), s.substr(i), true};
            }
        }
        if (s.size() < 3) return {{}, {}, false};
        return {s, {}, true};
    }
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '>') return {s.substr(1, i - 1), s.substr(i + 1), true};
    }
    return {{}, {}, false};
}

std::tuple<int, std::string_view, bool> tzsetOffset(std::string_view s) {
    if (s.empty()) return {0, {}, false};
    bool neg = false;
    if (s[0] == '+') {
        s.remove_prefix(1);
    } else if (s[0] == '-') {
        s.remove_prefix(1);
        neg = true;
    }
    // The tzdata code permits values up to 24 * 7 here, although POSIX
    // does not.
    auto [hours, rest, ok] = tzsetNum(s, 0, 24 * 7);
    if (!ok) return {0, {}, false};
    s = rest;
    int off = hours * secondsPerHour;
    if (s.empty() || s[0] != ':') return {neg ? -off : off, s, true};
    auto [mins, rest2, ok2] = tzsetNum(s.substr(1), 0, 59);
    if (!ok2) return {0, {}, false};
    s = rest2;
    off += mins * secondsPerMinute;
    if (s.empty() || s[0] != ':') return {neg ? -off : off, s, true};
    auto [secs, rest3, ok3] = tzsetNum(s.substr(1), 0, 59);
    if (!ok3) return {0, {}, false};
    s = rest3;
    off += secs;
    return {neg ? -off : off, s, true};
}

std::tuple<rule, std::string_view, bool> tzsetRule(std::string_view s) {
    rule r;
    if (s.empty()) return {rule{}, {}, false};
    if (s[0] == 'J') {
        auto [jday, rest, ok] = tzsetNum(s.substr(1), 1, 365);
        if (!ok) return {rule{}, {}, false};
        s = rest;
        r.kind = ruleJulian;
        r.day = jday;
    } else if (s[0] == 'M') {
        auto [mon, rest, ok] = tzsetNum(s.substr(1), 1, 12);
        if (!ok || rest.empty() || rest[0] != '.') return {rule{}, {}, false};
        auto [week, rest2, ok2] = tzsetNum(rest.substr(1), 1, 5);
        if (!ok2 || rest2.empty() || rest2[0] != '.') return {rule{}, {}, false};
        auto [day, rest3, ok3] = tzsetNum(rest2.substr(1), 0, 6);
        if (!ok3) return {rule{}, {}, false};
        s = rest3;
        r.kind = ruleMonthWeekDay;
        r.day = day;
        r.week = week;
        r.mon = mon;
    } else {
        auto [day, rest, ok] = tzsetNum(s, 0, 365);
        if (!ok) return {rule{}, {}, false};
        s = rest;
        r.kind = ruleDOY;
        r.day = day;
    }
    if (s.empty() || s[0] != '/') {
        r.time = 2 * secondsPerHour;  // 2am is the default
        return {r, s, true};
    }
    auto [offset, rest, ok] = tzsetOffset(s.substr(1));
    if (!ok) return {rule{}, {}, false};
    r.time = offset;
    return {r, rest, true};
}

// The time in seconds since the start of the year at which rule r takes
// effect in year, at offset off.
int tzruleTime(int year, const rule& r, int off) {
    int s = 0;
    switch (r.kind) {
    case ruleJulian:
        s = (r.day - 1) * secondsPerDay;
        if (isLeap(year) && r.day >= 60) s += secondsPerDay;
        break;
    case ruleDOY:
        s = r.day * secondsPerDay;
        break;
    case ruleMonthWeekDay: {
        // Zeller's Congruence.
        int m1 = (r.mon + 9) % 12 + 1;
        int yy0 = year;
        if (r.mon <= 2) yy0--;
        int yy1 = yy0 / 100;
        int yy2 = yy0 % 100;
        int dow = ((26 * m1 - 2) / 10 + 1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
        if (dow < 0) dow += 7;
        // Now dow is the day-of-week of the first day of r.mon. Get the
        // day-of-month of the first "dow" day.
        int d = r.day - dow;
        if (d < 0) d += 7;
        for (int i = 1; i < r.week; i++) {
            if (d + 7 >= daysIn(Month(r.mon), year)) break;
            d += 7;
        }
        d += daysBefore(Month(r.mon));
        if (isLeap(year) && r.mon > 2) d++;
        s = d * secondsPerDay;
        break;
    }
    }
    return s + r.time - off;
}

tzset_result tzset(std::string_view s, int64 lastTxSec, int64 sec) {
    tzset_result fail;
    auto [stdName, s1, ok] = tzsetName(s);
    int stdOffset = 0;
    if (ok) std::tie(stdOffset, s1, ok) = tzsetOffset(s1);
    if (!ok) return fail;
    s = s1;
    // The numbers in the tzset string are added to local time to get UTC,
    // but our offsets are added to UTC to get local time, so we negate the
    // number we see here.
    stdOffset = -stdOffset;
    if (s.empty() || s[0] == ',') {
        // No daylight savings time.
        return {stdName, stdOffset, lastTxSec, omega, false, true};
    }
    auto [dstName, s2, ok2] = tzsetName(s);
    int dstOffset = 0;
    if (ok2) {
        if (s2.empty() || s2[0] == ',') {
            dstOffset = stdOffset + secondsPerHour;
        } else {
            std::tie(dstOffset, s2, ok2) = tzsetOffset(s2);
            dstOffset = -dstOffset;  // as with stdOffset, above
        }
    }
    if (!ok2) return fail;
    s = s2;
    if (s.empty()) {
        // Default DST rules per tzcode.
        s = ",M3.2.0,M11.1.0";
    }
    // The TZ definition does not mention ';' here but tzcode accepts it.
    if (s[0] != ',' && s[0] != ';') return fail;
    s.remove_prefix(1);
    auto [startRule, s3, ok3] = tzsetRule(s);
    if (!ok3 || s3.empty() || s3[0] != ',') return fail;
    auto [endRule, s4, ok4] = tzsetRule(s3.substr(1));
    if (!ok4 || !s4.empty()) return fail;

    auto [year, yday] = absYearYday(static_cast<absSeconds>(sec + unixToInternal + internalToAbsolute) / secondsPerDay);
    int64 ysec = static_cast<int64>(yday - 1) * secondsPerDay + sec % secondsPerDay;
    // Compute start of year in seconds since Unix epoch.
    int64 ystart = sec - ysec;
    int64 startSec = tzruleTime(year, startRule, stdOffset);
    int64 endSec = tzruleTime(year, endRule, dstOffset);
    bool dstIsDST = true, stdIsDST = false;
    // Note: this is a flipping of "DST" and "STD" while retaining the
    // labels. This happens in southern hemispheres. The labelling here
    // thus is a little inconsistent with the goal.
    if (endSec < startSec) {
        std::swap(startSec, endSec);
        std::swap(stdName, dstName);
        std::swap(stdOffset, dstOffset);
        std::swap(stdIsDST, dstIsDST);
    }
    // The start and end values that we return are accurate close to a
    // daylight savings transition, but are otherwise just the start and end
    // of the year. That suffices for the caller.
    if (ysec < startSec) return {stdName, stdOffset, ystart, startSec + ystart, stdIsDST, true};
    if (ysec >= endSec) return {stdName, stdOffset, endSec + ystart, ystart + 365 * secondsPerDay, stdIsDST, true};
    return {dstName, dstOffset, startSec + ystart, endSec + ystart, dstIsDST, true};
}

}  // namespace

// ---------------------------------------------------------------------------
// FixedZone

namespace {

std::mutex& tablesMu() {
    static auto* m = new std::mutex;
    return *m;
}

Location* fixedZone(std::string_view name, int offset) {
    auto* l = new Location;
    string n = string(name);
    l->name = n;
    l->zones.push_back(Location::zone{n, offset, false});
    l->tx.push_back(Location::zoneTrans{alpha, 0, false, false});
    l->cacheStart = alpha;
    l->cacheEnd = omega;
    l->hasCache = true;
    l->cacheZone = l->zones[0];
    return l;
}

}  // namespace

const Location* FixedZone(std::string_view name, int offset) {
    static auto* table = new std::map<std::pair<std::string, int>, Location*>;
    std::lock_guard g(tablesMu());
    auto [it, inserted] = table->try_emplace({std::string(name), offset}, nullptr);
    if (inserted) it->second = fixedZone(name, offset);
    return it->second;
}

// ---------------------------------------------------------------------------
// Reading zone data (Go's zoneinfo_read.go)

namespace {

constexpr std::size_t maxFileSize = 10 << 20;

const error errBadData = errors::New("malformed time zone information");

// Go's dataIO: a reader of big-endian values.
struct dataIO {
    std::span<const byte> p;
    bool error = false;

    std::span<const byte> read(std::size_t n) {
        if (p.size() < n) {
            p = {};
            error = true;
            return {};
        }
        auto r = p.first(n);
        p = p.subspan(n);
        return r;
    }
    std::pair<uint32, bool> big4() {
        auto b = read(4);
        if (b.size() < 4) {
            error = true;
            return {0, false};
        }
        return {uint32(b[3]) | uint32(b[2]) << 8 | uint32(b[1]) << 16 | uint32(b[0]) << 24, true};
    }
    std::pair<uint64, bool> big8() {
        auto [n1, ok1] = big4();
        auto [n2, ok2] = big4();
        if (!ok1 || !ok2) {
            error = true;
            return {0, false};
        }
        return {(uint64(n1) << 32) | n2, true};
    }
    std::pair<byte, bool> byte1() {
        auto b = read(1);
        if (b.empty()) {
            error = true;
            return {0, false};
        }
        return {b[0], true};
    }
    std::span<const byte> rest() {
        auto r = p;
        p = {};
        return r;
    }
};

// The NUL-terminated string at the start of p.
std::string_view byteString(std::span<const byte> p) {
    std::string_view s(reinterpret_cast<const char*>(p.data()), p.size());
    if (auto i = s.find('\0'); i != s.npos) s = s.substr(0, i);
    return s;
}

int findZone(const std::vector<Location::zone>& zones, std::string_view name, int offset, bool isDST) {
    for (std::size_t i = 0; i < zones.size(); i++) {
        const auto& z = zones[i];
        if (z.name.view() == name && z.offset == offset && z.isDST == isDST) return static_cast<int>(i);
    }
    return -1;
}

// Go's LoadLocationFromTZData, into a new Location.
std::pair<Location*, error> parseTZData(std::string_view name, std::span<const byte> data) {
    dataIO d{data};

    // 4-byte magic "TZif"
    if (auto magic = d.read(4); std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()) != "TZif") {
        return {nullptr, errBadData};
    }

    // 1-byte version, then 15 bytes of padding
    int version = 0;
    auto p = d.read(16);
    if (p.size() != 16) return {nullptr, errBadData};
    switch (p[0]) {
    case 0: version = 1; break;
    case '2': version = 2; break;
    case '3': version = 3; break;
    default: return {nullptr, errBadData};
    }

    // six big-endian 32-bit integers:
    //	number of UTC/local indicators
    //	number of standard/wall indicators
    //	number of leap seconds
    //	number of transition times
    //	number of local time zones
    //	number of characters of time zone abbrev strings
    enum { NUTCLocal, NStdWall, NLeap, NTime, NZone, NChar };
    std::size_t n[6];
    for (int i = 0; i < 6; i++) {
        auto [nn, ok] = d.big4();
        if (!ok) return {nullptr, errBadData};
        n[i] = nn;
    }

    // If we have version 2 or 3, then the data is first written out in a
    // 32-bit format, then written out again in a 64-bit format. Skip the
    // 32-bit format and read the 64-bit one, as it can describe a broader
    // range of dates.
    bool is64 = false;
    if (version > 1) {
        // Skip the 32-bit data.
        std::size_t skip = n[NTime] * 4 + n[NTime] + n[NZone] * 6 + n[NChar] + n[NLeap] * 8 + n[NStdWall] + n[NUTCLocal];
        // Skip the version 2 header that we just read.
        skip += 4 + 16;
        d.read(skip);
        is64 = true;
        // Read the counts again, they can differ.
        for (int i = 0; i < 6; i++) {
            auto [nn, ok] = d.big4();
            if (!ok) return {nullptr, errBadData};
            n[i] = nn;
        }
    }

    std::size_t size = is64 ? 8 : 4;

    // Transition times.
    dataIO txtimes{d.read(n[NTime] * size)};
    // Time zone indices for transition times.
    auto txzones = d.read(n[NTime]);
    // Zone info structures
    dataIO zonedata{d.read(n[NZone] * 6)};
    // Time zone abbreviations.
    auto abbrev = d.read(n[NChar]);
    // Leap-second time pairs
    d.read(n[NLeap] * (size + 4));
    // Whether tx times associated with local time types are specified as
    // standard time or wall time.
    auto isstd = d.read(n[NStdWall]);
    // Whether tx times associated with local time types are specified as
    // UTC or local time.
    auto isutc = d.read(n[NUTCLocal]);

    if (d.error) return {nullptr, errBadData};  // ran out of data

    std::string_view extend;
    auto rest = d.rest();
    if (rest.size() > 2 && rest[0] == '\n' && rest[rest.size() - 1] == '\n') {
        extend = std::string_view(reinterpret_cast<const char*>(rest.data()) + 1, rest.size() - 2);
    }

    // Now we can build up a useful data structure. First the zone
    // information.
    //	utcoff[4] isdst[1] nameindex[1]
    std::size_t nzone = n[NZone];
    if (nzone == 0) {
        // Reject tzdata files with no zones. There's nothing useful in
        // them. This also avoids a panic later when we add and then use a
        // fake transition (golang.org/issue/29437).
        return {nullptr, errBadData};
    }
    auto* l = new Location;
    std::unique_ptr<Location> guard(l);
    l->zones.resize(nzone);
    for (auto& z : l->zones) {
        auto [off, ok] = zonedata.big4();
        if (!ok) return {nullptr, errBadData};
        z.offset = static_cast<int>(static_cast<int32>(off));
        auto [b, ok2] = zonedata.byte1();
        if (!ok2) return {nullptr, errBadData};
        z.isDST = b != 0;
        auto [b2, ok3] = zonedata.byte1();
        if (!ok3 || b2 >= abbrev.size()) return {nullptr, errBadData};
        z.name = string(byteString(abbrev.subspan(b2)));
    }

    // Now the transition time info.
    l->tx.resize(n[NTime]);
    for (std::size_t i = 0; i < l->tx.size(); i++) {
        int64 when;
        if (!is64) {
            auto [n4, ok] = txtimes.big4();
            if (!ok) return {nullptr, errBadData};
            when = static_cast<int64>(static_cast<int32>(n4));
        } else {
            auto [n8, ok] = txtimes.big8();
            if (!ok) return {nullptr, errBadData};
            when = static_cast<int64>(n8);
        }
        l->tx[i].when = when;
        if (txzones[i] >= l->zones.size()) return {nullptr, errBadData};
        l->tx[i].index = txzones[i];
        if (i < isstd.size()) l->tx[i].isstd = isstd[i] != 0;
        if (i < isutc.size()) l->tx[i].isutc = isutc[i] != 0;
    }

    if (l->tx.empty()) {
        // Build fake transition to cover all time. This happens in fixed
        // locations like "Etc/GMT0".
        l->tx.push_back(Location::zoneTrans{alpha, 0, false, false});
    }

    l->name = string(name);
    l->extend = string(extend);

    // Fill in the cache with information about right now, since that will
    // be the most common lookup.
    int64 sec = Now().Unix();
    const auto& tx = l->tx;
    for (std::size_t i = 0; i < tx.size(); i++) {
        if (tx[i].when <= sec && (i + 1 == tx.size() || sec < tx[i + 1].when)) {
            l->cacheStart = tx[i].when;
            l->cacheEnd = omega;
            l->hasCache = true;
            l->cacheZone = l->zones[tx[i].index];
            if (i + 1 < tx.size()) {
                l->cacheEnd = tx[i + 1].when;
            } else if (!l->extend.empty()) {
                // If we're at the end of the known zone transitions, try
                // the extend string.
                auto e = tzset(l->extend, l->cacheStart, sec);
                if (e.ok) {
                    l->cacheStart = e.start;
                    l->cacheEnd = e.end;
                    // Find the zone that is returned by tzset to avoid
                    // allocation if possible.
                    if (int zi = findZone(l->zones, e.name, e.offset, e.isDST); zi != -1) {
                        l->cacheZone = l->zones[zi];
                    } else {
                        l->cacheZone = Location::zone{string::fromStatic(e.name), e.offset, e.isDST};
                    }
                }
            }
            break;
        }
    }
    // tzset's names are views into extend: make the copies the Location's
    // own (string copies share the buffer, which lives as long as l).
    if (l->hasCache && findZone(l->zones, l->cacheZone.name, l->cacheZone.offset, l->cacheZone.isDST) == -1) {
        l->cacheZone.name = string(l->cacheZone.name.view());
    }
    return {guard.release(), nullptr};
}

// The Locations of LoadLocation and LoadLocationFromTZData, by name and
// data: loading the same zone again returns the same Location.
const Location* internTZData(std::string_view name, std::span<const byte> data, Location* fresh) {
    static auto* table = new std::map<std::pair<std::string, std::string>, Location*>;
    std::lock_guard g(tablesMu());
    auto key = std::pair(std::string(name), std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    auto [it, inserted] = table->try_emplace(std::move(key), fresh);
    if (!inserted) delete fresh;
    return it->second;
}

// A file's contents, or the errno of the failure (with a file size error
// as EFBIG).
struct file_data {
    std::string data;
    error err;
    int errnum = 0;  // the errno, for ENOENT checks
};

file_data failErrno(int e) { return {{}, syscall::errnoError(e), e}; }

file_data readFile(const std::string& name) {
    int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return failErrno(errno);
    file_data r;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            return failErrno(e);
        }
        if (n == 0) break;
        r.data.append(buf, static_cast<std::size_t>(n));
        if (r.data.size() > maxFileSize) {
            ::close(fd);
            return {{}, errors::New("time: file " + string(name) + " is too large"), EFBIG};
        }
    }
    ::close(fd);
    return r;
}

// Reads exactly buf.size() bytes at off (from the end if negative).
bool preadn(int fd, std::span<char> buf, int64 off) {
    if (off < 0) {
        off += ::lseek(fd, 0, SEEK_END);
        if (off < 0) return false;
    }
    while (!buf.empty()) {
        ssize_t m = ::pread(fd, buf.data(), buf.size(), off);
        if (m < 0 && errno == EINTR) continue;
        if (m <= 0) return false;
        buf = buf.subspan(static_cast<std::size_t>(m));
        off += m;
    }
    return true;
}

int get4(std::span<const char> b) {
    if (b.size() < 4) return 0;
    auto u = [&](std::size_t i) { return static_cast<int>(static_cast<unsigned char>(b[i])); };
    return u(0) | u(1) << 8 | u(2) << 16 | u(3) << 24;
}

int get2(std::span<const char> b) {
    if (b.size() < 2) return 0;
    auto u = [&](std::size_t i) { return static_cast<int>(static_cast<unsigned char>(b[i])); };
    return u(0) | u(1) << 8;
}

// Go's loadTzinfoFromZip: the uncompressed file name in zipfile.
file_data loadTzinfoFromZip(const std::string& zipfile, const std::string& name) {
    int fd = ::open(zipfile.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return failErrno(errno);
    struct closer {
        int fd;
        ~closer() { ::close(fd); }
    } c{fd};
    auto corrupt = [&] { return file_data{{}, errors::New("corrupt zip file " + string(zipfile)), EINVAL}; };

    constexpr int zecheader = 0x06054b50;
    constexpr int zcheader = 0x02014b50;
    constexpr int ztailsize = 22;
    constexpr int zheadersize = 30;
    constexpr int zheader = 0x04034b50;

    std::string buf(ztailsize, '\0');
    if (!preadn(fd, buf, -ztailsize) || get4(buf) != zecheader) return corrupt();
    int n = get2(std::span(buf).subspan(10));
    int size = get4(std::span(buf).subspan(12));
    int off = get4(std::span(buf).subspan(16));

    buf.assign(static_cast<std::size_t>(size), '\0');
    if (!preadn(fd, buf, off)) return corrupt();
    std::span<const char> b(buf);
    for (int i = 0; i < n; i++) {
        // zip entry layout:
        //	0	magic[4]
        //	4	madevers[1]
        //	5	madeos[1]
        //	6	extvers[1]
        //	7	extos[1]
        //	8	flags[2]
        //	10	meth[2]
        //	12	modtime[2]
        //	14	moddate[2]
        //	16	crc[4]
        //	20	csize[4]
        //	24	uncsize[4]
        //	28	namelen[2]
        //	30	xlen[2]
        //	32	fclen[2]
        //	34	disknum[2]
        //	36	iattr[2]
        //	38	eattr[4]
        //	42	off[4]
        //	46	name[namelen]
        //	46+namelen+xlen+fclen - next header
        if (b.size() < 46 || get4(b) != zcheader) break;
        int meth = get2(b.subspan(10));
        int fsize = get4(b.subspan(24));
        int namelen = get2(b.subspan(28));
        int xlen = get2(b.subspan(30));
        int fclen = get2(b.subspan(32));
        int foff = get4(b.subspan(42));
        if (b.size() < static_cast<std::size_t>(46 + namelen + xlen + fclen)) break;
        std::string_view zname(b.data() + 46, static_cast<std::size_t>(namelen));
        b = b.subspan(static_cast<std::size_t>(46 + namelen + xlen + fclen));
        if (zname != name) continue;
        if (meth != 0) {
            return {{}, errors::New("unsupported compression for " + string(name) + " in " + string(zipfile)), EINVAL};
        }
        // zip per-file header layout:
        //	0	magic[4]
        //	4	extvers[1]
        //	5	extos[1]
        //	6	flags[2]
        //	8	meth[2]
        //	10	modtime[2]
        //	12	moddate[2]
        //	14	crc[4]
        //	18	csize[4]
        //	22	uncsize[4]
        //	26	namelen[2]
        //	28	xlen[2]
        //	30	name[namelen]
        //	30+namelen+xlen - file data
        std::string h(static_cast<std::size_t>(zheadersize + namelen), '\0');
        if (!preadn(fd, h, foff) || get4(h) != zheader || get2(std::span(h).subspan(8)) != meth ||
            get2(std::span(h).subspan(26)) != namelen || std::string_view(h).substr(30, namelen) != name) {
            return corrupt();
        }
        xlen = get2(std::span(h).subspan(28));
        file_data r;
        r.data.assign(static_cast<std::size_t>(fsize), '\0');
        if (!preadn(fd, r.data, foff + 30 + namelen + xlen)) return corrupt();
        return r;
    }
    return failErrno(ENOENT);
}

file_data loadTzinfoFromDirOrZip(const std::string& dir, std::string name) {
    if (dir.size() > 4 && dir.ends_with(".zip")) return loadTzinfoFromZip(dir, name);
    if (!dir.empty()) name = dir + "/" + name;
    return readFile(name);
}

std::span<const byte> as_bytes(const std::string& s) {
    return {reinterpret_cast<const byte*>(s.data()), s.size()};
}

// Go's loadLocation: the first source that has name.
std::pair<const Location*, error> loadLocation(std::string_view name, const std::vector<std::string>& sources) {
    error firstErr;
    for (const auto& source : sources) {
        auto zd = loadTzinfoFromDirOrZip(source, std::string(name));
        error err = zd.err;
        if (!err) {
            auto [z, perr] = parseTZData(name, as_bytes(zd.data));
            if (!perr) return {internTZData(name, as_bytes(zd.data), z), nullptr};
            err = perr;
            zd.errnum = 0;
        }
        if (!firstErr && zd.errnum != ENOENT) firstErr = err;
    }
    if (firstErr) return {nullptr, firstErr};
    return {nullptr, errors::New("unknown time zone " + string(name))};
}

const std::vector<std::string>& platformZoneSources() {
    static const auto* s = new std::vector<std::string>{
        "/usr/share/zoneinfo/",
        "/usr/share/lib/zoneinfo/",
        "/usr/lib/locale/TZ/",
        "/etc/zoneinfo",
    };
    return *s;
}

// ForceZipFileForTesting's zip file; empty for the system's sources.
std::string& forcedZip() {
    static auto* s = new std::string;
    return *s;
}

std::vector<std::string> zoneSources() {
    std::lock_guard g(tablesMu());
    if (!forcedZip().empty()) return {forcedZip()};
    return platformZoneSources();
}

// Go's initLocal (zoneinfo_unix.go).
void initLocal() {
    // consult $TZ to find the time zone to use.
    // no $TZ means use the system default /etc/localtime.
    // $TZ="" means use UTC.
    // $TZ="foo" or $TZ=":foo" if foo is an absolute path, then the file
    // pointed by foo will be used to initialize timezone; otherwise, file
    // /usr/share/zoneinfo/foo will be used.
    Location& local = localLoc();
    const char* env = std::getenv("TZ");
    if (!env) {
        auto [z, err] = loadLocation("localtime", {"/etc"});
        if (!err) {
            local = *z;
            local.name = string::fromStatic("Local");
            return;
        }
    } else if (*env) {
        std::string_view tz = env;
        if (tz[0] == ':') tz.remove_prefix(1);
        if (!tz.empty() && tz[0] == '/') {
            auto [z, err] = loadLocation(tz, {""});
            if (!err) {
                local = *z;
                local.name = tz == "/etc/localtime" ? string::fromStatic("Local") : string(tz);
                return;
            }
        } else if (!tz.empty() && tz != "UTC") {
            auto [z, err] = loadLocation(tz, zoneSources());
            if (!err) {
                local = *z;
                return;
            }
        }
    }
    // Fall back to UTC.
    local = Location();
    local.name = string::fromStatic("UTC");
}

bool containsDotDot(std::string_view s) { return s.find("..") != s.npos; }

const error errLocation = errors::New("time: invalid location name");

}  // namespace

std::pair<const Location*, error> LoadLocationFromTZData(std::string_view name, std::span<const byte> data) {
    auto [l, err] = parseTZData(name, data);
    if (err) return {nullptr, err};
    return {internTZData(name, data, l), nullptr};
}

std::pair<const Location*, error> LoadLocation(std::string_view name) {
    if (name.empty() || name == "UTC") return {UTC, nullptr};
    if (name == "Local") return {Local, nullptr};
    if (containsDotDot(name) || name[0] == '/' || name[0] == '\\') {
        // No valid IANA Time Zone name contains a single dot, much less
        // dot dot. Likewise, none begin with a slash.
        return {nullptr, errLocation};
    }
    error firstErr;
    if (const char* zoneinfo = std::getenv("ZONEINFO"); zoneinfo && *zoneinfo) {
        auto zd = loadTzinfoFromDirOrZip(zoneinfo, std::string(name));
        if (!zd.err) {
            auto [z, err] = LoadLocationFromTZData(name, as_bytes(zd.data));
            if (!err) return {z, nullptr};
            firstErr = err;
        } else if (zd.errnum != ENOENT) {
            firstErr = zd.err;
        }
    }
    auto [z, err] = loadLocation(name, zoneSources());
    if (!err) return {z, nullptr};
    if (!firstErr) firstErr = err;
    return {nullptr, firstErr};
}

// ---------------------------------------------------------------------------
// Test hooks

namespace testing_hooks {

void ForceUSPacificForTesting() {
    std::lock_guard l(localMu());
    auto [z, err] = loadLocation("America/Los_Angeles", zoneSources());
    if (err) panic("cannot load America/Los_Angeles for testing: " + err.Error());
    localLoc() = *z;
    localLoc().name = string::fromStatic("Local");
    local_done.store(true, std::memory_order_release);
}

void ResetLocalOnceForTest() {
    std::lock_guard l(localMu());
    localLoc() = Location();
    local_done.store(false, std::memory_order_release);
}

void ForceZipFileForTesting(std::string_view zip) {
    std::lock_guard g(tablesMu());
    forcedZip() = std::string(zip);
}

TzsetResult Tzset(std::string_view s, int64 lastTxSec, int64 sec) {
    auto r = tzset(s, lastTxSec, sec);
    return {std::string(r.name), r.offset, r.start, r.end, r.isDST, r.ok};
}

std::tuple<std::string, std::string, bool> TzsetName(std::string_view s) {
    auto [a, b, ok] = tzsetName(s);
    return {std::string(a), std::string(b), ok};
}

std::tuple<int, std::string, bool> TzsetOffset(std::string_view s) {
    auto [a, b, ok] = tzsetOffset(s);
    return {a, std::string(b), ok};
}

std::tuple<Rule, std::string, bool> TzsetRule(std::string_view s) {
    auto [r, rest, ok] = tzsetRule(s);
    return {Rule{r.kind, r.day, r.week, r.mon, r.time}, std::string(rest), ok};
}

const error ErrLocation = errLocation;

std::pair<std::string, error> ReadFile(const std::string& name) {
    auto r = readFile(name);
    return {std::move(r.data), r.err};
}

}  // namespace testing_hooks

}  // namespace gopp::time
