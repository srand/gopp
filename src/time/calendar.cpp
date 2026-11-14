// Time's calendar and arithmetic, after Go 1.27.3's time.go.
#include "internal.hpp"

#include <gopp/base.hpp>
#include <gopp/errors.hpp>

#include <bit>

namespace gopp::time {

// ---------------------------------------------------------------------------
// Months and weekdays

const char* const longDayNames[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const shortDayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const shortMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const longMonthNames[12] = {"January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December"};

string Month::String() const {
    if (January <= *this && *this <= December) return string::fromStatic(longMonthNames[value - 1]);
    return "%!Month(" + string(std::to_string(static_cast<uint64>(static_cast<int64>(value)))) + ")";
}

string Weekday::String() const {
    if (Sunday <= *this && *this <= Saturday) return string::fromStatic(longDayNames[value]);
    return "%!Weekday(" + string(std::to_string(static_cast<uint64>(static_cast<int64>(value)))) + ")";
}

// ---------------------------------------------------------------------------
// The absolute calendar (Go's absSeconds, absDays, ...): Neri and
// Schneider's algorithms over years that start on March 1.

namespace {

struct splitDays {
    uint64 century;
    uint32 cyear;
    uint32 ayday;  // day of the March-based year, 0-based
};

splitDays daysSplit(absDays days) {
    uint64 d = 4 * days + 3;
    uint64 century = d / 146097;
    uint32 cd = static_cast<uint32>(d % 146097) | 3;
    uint64 prod = uint64(2939745) * cd;
    uint32 hi = static_cast<uint32>(prod >> 32), lo = static_cast<uint32>(prod);
    return {century, hi, lo / 2939745 / 4};
}

// The March-based month (3..14) and day of month.
std::pair<int, int> aydaySplit(uint32 ayday) {
    uint32 d = 2141 * ayday + 197913;
    return {static_cast<int>(d >> 16), 1 + static_cast<int>((d & 0xFFFF) / 2141)};
}

int aydayJanFeb(uint32 ayday) { return ayday >= marchThruDecember ? 1 : 0; }

int centuryLeap(uint64 century, uint32 cyear) {
    int y4ok = cyear % 4 == 0 ? 1 : 0;
    int y100ok = cyear != 0 ? 1 : 0;
    int y400ok = century % 4 == 0 ? 1 : 0;
    return y4ok & (y100ok | y400ok);
}

int centuryYear(uint64 century, uint32 cyear, int janFeb) {
    return static_cast<int>(static_cast<int64>(century * 100 - absoluteYears)) + static_cast<int>(cyear) + janFeb;
}

int aydayYday(uint32 ayday, int janFeb, int leap) {
    return static_cast<int>(ayday) + (1 + 31 + 28) + (leap & ~janFeb) - 365 * janFeb;
}

}  // namespace

absDays dateToAbsDays(int64 year, Month month, int day) {
    uint32 amonth = static_cast<uint32>(month.value);
    uint32 janFeb = amonth < 3 ? 1 : 0;
    amonth += 12 * janFeb;
    uint64 y = static_cast<uint64>(year) - janFeb + absoluteYears;
    uint32 ayday = (979 * amonth - 2919) >> 5;
    uint64 century = y / 100;
    uint32 cyear = static_cast<uint32>(y % 100);
    uint32 cday = 1461 * cyear / 4;
    uint64 centurydays = 146097 * century / 4;
    return centurydays + static_cast<uint64>(static_cast<int64>(cday + ayday) + static_cast<int64>(day) - 1);
}

std::tuple<int, Month, int> absDate(absDays days) {
    auto [century, cyear, ayday] = daysSplit(days);
    auto [amonth, day] = aydaySplit(ayday);
    int janFeb = aydayJanFeb(ayday);
    return {centuryYear(century, cyear, janFeb), Month(amonth - janFeb * 12), day};
}

std::pair<int, int> absYearYday(absDays days) {
    auto [century, cyear, ayday] = daysSplit(days);
    int janFeb = aydayJanFeb(ayday);
    return {centuryYear(century, cyear, janFeb), aydayYday(ayday, janFeb, centuryLeap(century, cyear))};
}

Weekday absWeekday(absDays days) { return Weekday(static_cast<int>((days + uint64(Wednesday.value)) % 7)); }

std::tuple<int, int, int> absClock(absSeconds abs) {
    int sec = static_cast<int>(abs % secondsPerDay);
    int hour = sec / secondsPerHour;
    sec -= hour * secondsPerHour;
    int min = sec / secondsPerMinute;
    sec -= min * secondsPerMinute;
    return {hour, min, sec};
}

bool isLeap(int year) {
    // year%4 == 0 && (year%100 != 0 || year%400 == 0), as Go computes it.
    int mask = year % 25 != 0 ? 3 : 0xf;
    return (year & mask) == 0;
}

int daysBefore(Month m) {
    int adj = m >= March ? -2 : 0;
    return (214 * m.value - 211) / 7 + adj;
}

int daysIn(Month m, int year) {
    if (m == February) return isLeap(year) ? 29 : 28;
    return 30 + ((m.value + (m.value >> 3)) & 1);
}

// ---------------------------------------------------------------------------
// Time's internals

void Time::internal::addSec(Time& t, int64 d) noexcept {
    if (t.hasMono_) {
        // The reading stays only while the wall time is in its range.
        int64 dsec = static_cast<int64>(static_cast<uint64>(t.sec_) + static_cast<uint64>(d));
        if (((dsec > t.sec_) == (d > 0) || d == 0) && minWall <= dsec && dsec <= maxWall) {
            t.sec_ = dsec;
            return;
        }
        stripMono(t);
    }
    int64 sum = static_cast<int64>(static_cast<uint64>(t.sec_) + static_cast<uint64>(d));
    if ((sum > t.sec_) == (d > 0)) t.sec_ = sum;
    else if (d > 0) t.sec_ = INT64_MAX;
    else t.sec_ = -INT64_MAX;
}

void Time::internal::setLoc(Time& t, const time::Location* loc) noexcept {
    if (loc == time::UTC) loc = nullptr;
    stripMono(t);
    t.loc_ = loc;
}

absSeconds Time::internal::absSec(const Time& t) {
    const time::Location* l = t.loc_ ? t.loc_->get() : time::UTC;
    int64 sec = unixSec(t);
    if (l != time::UTC) {
        if (l->hasCache && l->cacheStart <= sec && sec < l->cacheEnd) sec += l->cacheZone.offset;
        else sec += l->lookup(sec).offset;
    }
    return static_cast<absSeconds>(sec + (unixToInternal + internalToAbsolute));
}

std::tuple<string, int, absSeconds> Time::internal::locabs(const Time& t) {
    const time::Location* l = t.loc_ ? t.loc_->get() : time::UTC;
    int64 sec = unixSec(t);
    string name;
    int offset = 0;
    if (l != time::UTC) {
        if (l->hasCache && l->cacheStart <= sec && sec < l->cacheEnd) {
            name = l->cacheZone.name;
            offset = l->cacheZone.offset;
        } else {
            auto r = l->lookup(sec);
            name = r.name;
            offset = r.offset;
        }
        sec += offset;
    } else {
        name = string::fromStatic("UTC");
    }
    return {name, offset, static_cast<absSeconds>(sec + (unixToInternal + internalToAbsolute))};
}

// ---------------------------------------------------------------------------
// Comparison and arithmetic

bool Time::After(const Time& u) const noexcept {
    if (hasMono_ && u.hasMono_) return mono_ > u.mono_;
    return sec_ > u.sec_ || (sec_ == u.sec_ && nsec_ > u.nsec_);
}

bool Time::Before(const Time& u) const noexcept {
    if (hasMono_ && u.hasMono_) return mono_ < u.mono_;
    return sec_ < u.sec_ || (sec_ == u.sec_ && nsec_ < u.nsec_);
}

int Time::Compare(const Time& u) const noexcept {
    int64 tc, uc;
    if (hasMono_ && u.hasMono_) {
        tc = mono_;
        uc = u.mono_;
    } else {
        tc = sec_;
        uc = u.sec_;
        if (tc == uc) {
            tc = nsec_;
            uc = u.nsec_;
        }
    }
    return tc < uc ? -1 : tc > uc ? 1 : 0;
}

bool Time::Equal(const Time& u) const noexcept {
    if (hasMono_ && u.hasMono_) return mono_ == u.mono_;
    return sec_ == u.sec_ && nsec_ == u.nsec_;
}

Time Time::Add(Duration d) const noexcept {
    Time t = *this;
    int64 dsec = d.ns / 1000000000;
    int32 nsec = t.nsec_ + static_cast<int32>(d.ns % 1000000000);
    if (nsec >= 1000000000) {
        dsec++;
        nsec -= 1000000000;
    } else if (nsec < 0) {
        dsec--;
        nsec += 1000000000;
    }
    t.nsec_ = nsec;
    internal::addSec(t, dsec);
    if (t.hasMono_) {
        int64 te = static_cast<int64>(static_cast<uint64>(t.mono_) + static_cast<uint64>(d.ns));
        if ((d.ns < 0 && te > t.mono_) || (d.ns > 0 && te < t.mono_)) internal::stripMono(t);
        else t.mono_ = te;
    }
    return t;
}

namespace {

constexpr Duration minDuration{INT64_MIN};
constexpr Duration maxDuration{INT64_MAX};

Duration subMono(int64 t, int64 u) {
    Duration d(static_cast<int64>(static_cast<uint64>(t) - static_cast<uint64>(u)));
    if (d.ns < 0 && t > u) return maxDuration;
    if (d.ns > 0 && t < u) return minDuration;
    return d;
}

bool lessThanHalf(Duration x, Duration y) {
    return static_cast<uint64>(x.ns) + static_cast<uint64>(x.ns) < static_cast<uint64>(y.ns);
}

}  // namespace

Duration Time::Sub(const Time& u) const noexcept {
    if (hasMono_ && u.hasMono_) return subMono(mono_, u.mono_);
    Duration d(static_cast<int64>(static_cast<uint64>(sec_ - u.sec_) * 1000000000 +
                                  static_cast<uint64>(static_cast<int64>(nsec_ - u.nsec_))));
    if (u.Add(d).Equal(*this)) return d;
    if (Before(u)) return minDuration;
    return maxDuration;
}

Duration Since(const Time& t) noexcept { return Now().Sub(t); }

Duration Until(const Time& t) noexcept { return t.Sub(Now()); }

Duration Duration::Truncate(Duration m) const noexcept {
    if (m.ns <= 0) return *this;
    return Duration(ns - ns % m.ns);
}

Duration Duration::Round(Duration m) const noexcept {
    if (m.ns <= 0) return *this;
    Duration r(ns % m.ns);
    if (ns < 0) {
        r = Duration(-r.ns);
        if (lessThanHalf(r, m)) return Duration(ns + r.ns);
        int64 d1 = static_cast<int64>(static_cast<uint64>(ns) - static_cast<uint64>(m.ns) + static_cast<uint64>(r.ns));
        if (d1 < ns) return Duration(d1);
        return minDuration;
    }
    if (lessThanHalf(r, m)) return Duration(ns - r.ns);
    int64 d1 = static_cast<int64>(static_cast<uint64>(ns) + static_cast<uint64>(m.ns) - static_cast<uint64>(r.ns));
    if (d1 > ns) return Duration(d1);
    return maxDuration;
}

Duration Duration::Abs() const noexcept {
    if (ns >= 0) return *this;
    if (ns == INT64_MIN) return maxDuration;
    return Duration(-ns);
}

Time Time::AddDate(int years, int months, int days) const {
    auto [year, month, day] = Date();
    auto [hour, min, sec] = Clock();
    return time::Date(year + years, month + months, day + days, hour, min, sec, nsec_, Location());
}

// ---------------------------------------------------------------------------
// Calendar fields

std::tuple<int, time::Month, int> Time::Date() const { return absDate(internal::absSec(*this) / secondsPerDay); }

int Time::Year() const { return std::get<0>(Date()); }

time::Month Time::Month() const { return std::get<1>(Date()); }

int Time::Day() const { return std::get<2>(Date()); }

time::Weekday Time::Weekday() const { return absWeekday(internal::absSec(*this) / secondsPerDay); }

std::pair<int, int> Time::ISOWeek() const {
    // Weeks begin on Monday; the first week of a year has its Thursday.
    absDays days = internal::absSec(*this) / secondsPerDay;
    absDays thu = days + static_cast<absDays>(static_cast<int64>(Thursday.value) -
                                              (static_cast<int64>(absWeekday(days - 1).value) + 1));
    auto [year, yday] = absYearYday(thu);
    return {year, (yday - 1) / 7 + 1};
}

std::tuple<int, int, int> Time::Clock() const { return absClock(internal::absSec(*this)); }

int Time::Hour() const { return static_cast<int>(internal::absSec(*this) % secondsPerDay) / secondsPerHour; }

int Time::Minute() const { return static_cast<int>(internal::absSec(*this) % secondsPerHour) / secondsPerMinute; }

int Time::Second() const { return static_cast<int>(internal::absSec(*this) % secondsPerMinute); }

int Time::YearDay() const { return absYearYday(internal::absSec(*this) / secondsPerDay).second; }

// ---------------------------------------------------------------------------
// Locations

Time Time::UTC() const noexcept {
    Time t = *this;
    internal::setLoc(t, time::UTC);
    return t;
}

Time Time::Local() const noexcept {
    Time t = *this;
    internal::setLoc(t, time::Local);
    return t;
}

Time Time::In(const time::Location* loc) const {
    if (!loc) panic("time: missing Location in call to Time.In");
    Time t = *this;
    internal::setLoc(t, loc);
    return t;
}

const time::Location* Time::Location() const noexcept { return loc_ ? loc_ : time::UTC; }

std::pair<string, int> Time::Zone() const {
    auto r = Location()->lookup(internal::unixSec(*this));
    return {r.name, r.offset};
}

std::pair<Time, Time> Time::ZoneBounds() const {
    auto r = Location()->lookup(internal::unixSec(*this));
    Time start, end;
    if (r.start != alpha) {
        start = internal::unixTime(r.start, 0);
        internal::setLoc(start, loc_);
    }
    if (r.end != omega) {
        end = internal::unixTime(r.end, 0);
        internal::setLoc(end, loc_);
    }
    return {start, end};
}

bool Time::IsDST() const { return Location()->lookup(Unix()).isDST; }

// ---------------------------------------------------------------------------
// Construction

Time Unix(int64 sec, int64 nsec) noexcept {
    if (nsec < 0 || nsec >= 1000000000) {
        int64 n = nsec / 1000000000;
        sec += n;
        nsec -= n * 1000000000;
        if (nsec < 0) {
            nsec += 1000000000;
            sec--;
        }
    }
    return Time::internal::unixTime(sec, static_cast<int32>(nsec));
}

Time Time::Unix(int64 sec, int64 nsec) noexcept { return time::Unix(sec, nsec); }

Time UnixMilli(int64 msec) noexcept { return Unix(msec / 1000, (msec % 1000) * 1000000); }

Time UnixMicro(int64 usec) noexcept { return Unix(usec / 1000000, (usec % 1000000) * 1000); }

namespace {

// Go's norm: hi and lo with lo in [0, base).
void norm(int& hi, int& lo, int base) {
    if (lo < 0) {
        int n = (-lo - 1) / base + 1;
        hi -= n;
        lo += n * base;
    }
    if (lo >= base) {
        int n = lo / base;
        hi += n;
        lo -= n * base;
    }
}

}  // namespace

Time Date(int year, Month month, int day, int hour, int min, int sec, int nsec, const Location* loc) {
    if (!loc) panic("time: missing Location in call to Date");
    int m = month.value - 1;
    norm(year, m, 12);
    month = Month(m + 1);
    norm(sec, nsec, 1000000000);
    norm(min, sec, 60);
    norm(hour, min, 60);
    norm(day, hour, 24);

    int64 unix = static_cast<int64>(dateToAbsDays(year, month, day)) * secondsPerDay +
                 static_cast<int64>(hour * secondsPerHour + min * secondsPerMinute + sec) + absoluteToUnix;

    // Look for the zone offset for the expected time, so that we can adjust
    // to UTC. The lookup function expects UTC, so first guess with unix and
    // then look again after the adjustment.
    auto r = loc->lookup(unix);
    int offset = r.offset;
    if (offset != 0) {
        int64 utc = unix - offset;
        // If utc is valid for the time zone we found, then we have the right
        // offset. If not, we get the correct offset by looking up utc.
        if (utc < r.start || utc >= r.end) offset = loc->lookup(utc).offset;
        unix -= offset;
    }
    Time t = Time::internal::unixTime(unix, static_cast<int32>(nsec));
    Time::internal::setLoc(t, loc);
    return t;
}

// ---------------------------------------------------------------------------
// Truncate and Round

namespace {

// Go's div: t mod d, and whether t/d is odd. Only the remainder is used.
Duration div(const Time& t, Duration d) {
    bool neg = false;
    int32 nsec = Time::internal::nsec(t);
    int64 sec = Time::internal::sec(t);
    if (sec < 0) {
        // Operate on absolute value.
        neg = true;
        sec = -sec;
        nsec = -nsec;
        if (nsec < 0) {
            nsec += 1000000000;
            sec--;  // sec >= 1 before the -- so safe
        }
    }
    Duration r;
    if (d.ns < 1000000000 && 1000000000 % (d.ns + d.ns) == 0) {
        // Special case: 2d divides 1 second.
        r = Duration(nsec % static_cast<int32>(d.ns));
    } else if (d.ns % 1000000000 == 0) {
        // Special case: d is a multiple of 1 second.
        int64 d1 = d.ns / 1000000000;
        r = Duration((sec % d1) * 1000000000 + nsec);
    } else {
        // General case: compute (sec*1e9 + nsec) mod d in 128 bits.
        __extension__ typedef unsigned __int128 uint128;
        uint128 u = static_cast<uint128>(static_cast<uint64>(sec)) * 1000000000 + static_cast<uint64>(nsec);
        r = Duration(static_cast<int64>(u % static_cast<uint64>(d.ns)));
    }
    if (neg && r.ns != 0) r = Duration(d.ns - r.ns);
    return r;
}

}  // namespace

Time Time::Truncate(Duration d) const {
    Time t = *this;
    internal::stripMono(t);
    if (d.ns <= 0) return t;
    Duration r = div(t, d);
    return t.Add(Duration(-r.ns));
}

Time Time::Round(Duration d) const {
    Time t = *this;
    internal::stripMono(t);
    if (d.ns <= 0) return t;
    Duration r = div(t, d);
    if (lessThanHalf(r, d)) return t.Add(Duration(-r.ns));
    return t.Add(Duration(d.ns - r.ns));
}

}  // namespace gopp::time
