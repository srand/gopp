// The time package: durations, wall and monotonic time, sleeping and
// timers that work with channels.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/chan.hpp>
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <chrono>
#include <compare>
#include <format>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#endif

namespace gopp::time {

// time.Duration, in nanoseconds. Converts from std::chrono durations, so
// time::Sleep(100ms) works with std::chrono_literals.
struct Duration {
    int64 ns = 0;

    constexpr Duration() noexcept = default;
    constexpr explicit Duration(int64 n) noexcept : ns(n) {}
    template <typename Rep, typename Period>
    constexpr Duration(std::chrono::duration<Rep, Period> d) noexcept
        : ns(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) {}

    constexpr int64 Nanoseconds() const noexcept { return ns; }
    constexpr int64 Microseconds() const noexcept { return ns / 1000; }
    constexpr int64 Milliseconds() const noexcept { return ns / 1000000; }
    constexpr double Seconds() const noexcept {
        return static_cast<double>(ns / 1000000000) + static_cast<double>(ns % 1000000000) / 1e9;
    }
    constexpr double Minutes() const noexcept {
        return static_cast<double>(ns / (60 * 1000000000LL)) + static_cast<double>(ns % (60 * 1000000000LL)) / (60 * 1e9);
    }
    constexpr double Hours() const noexcept {
        return static_cast<double>(ns / (3600 * 1000000000LL)) +
               static_cast<double>(ns % (3600 * 1000000000LL)) / (60 * 60 * 1e9);
    }
    // d rounded toward zero, or to the nearest (halfway away from zero), to
    // a multiple of m; d unchanged if m <= 0.
    Duration Truncate(Duration m) const noexcept;
    Duration Round(Duration m) const noexcept;
    // |d|, with the most negative Duration giving the most positive.
    Duration Abs() const noexcept;
    string String() const;

    friend constexpr Duration operator+(Duration a, Duration b) noexcept { return Duration(a.ns + b.ns); }
    friend constexpr Duration operator-(Duration a, Duration b) noexcept { return Duration(a.ns - b.ns); }
    friend constexpr Duration operator-(Duration a) noexcept { return Duration(-a.ns); }
    friend constexpr Duration operator*(Duration a, int64 k) noexcept { return Duration(a.ns * k); }
    friend constexpr Duration operator*(int64 k, Duration a) noexcept { return Duration(a.ns * k); }
    friend constexpr Duration operator/(Duration a, int64 k) noexcept { return Duration(a.ns / k); }
    friend constexpr int64 operator/(Duration a, Duration b) noexcept { return a.ns / b.ns; }
    friend constexpr Duration operator%(Duration a, Duration b) noexcept { return Duration(a.ns % b.ns); }
    friend constexpr auto operator<=>(Duration a, Duration b) noexcept = default;
    constexpr Duration& operator+=(Duration o) noexcept {
        ns += o.ns;
        return *this;
    }
};

inline constexpr Duration Nanosecond{1};
inline constexpr Duration Microsecond{1000};
inline constexpr Duration Millisecond{1000000};
inline constexpr Duration Second{1000000000};
inline constexpr Duration Minute{60 * 1000000000LL};
inline constexpr Duration Hour{3600 * 1000000000LL};

// time.ParseDuration: "300ms", "-1.5h", "2h45m". Units: ns, us (or µs),
// ms, s, m, h.
std::pair<Duration, error> ParseDuration(std::string_view s);

class Location;
class Time;

// time.Month: January is 1.
struct Month {
    int value = 0;

    constexpr Month() noexcept = default;
    constexpr explicit Month(int v) noexcept : value(v) {}
    constexpr explicit operator int() const noexcept { return value; }
    // "January", or "%!Month(13)" out of range, as in Go.
    string String() const;

    friend constexpr auto operator<=>(Month a, Month b) noexcept = default;
    friend constexpr Month operator+(Month m, int n) noexcept { return Month(m.value + n); }
    friend constexpr Month operator+(Month a, Month b) noexcept { return Month(a.value + b.value); }
    friend constexpr Month operator-(Month m, int n) noexcept { return Month(m.value - n); }
};

inline constexpr Month January{1};
inline constexpr Month February{2};
inline constexpr Month March{3};
inline constexpr Month April{4};
inline constexpr Month May{5};
inline constexpr Month June{6};
inline constexpr Month July{7};
inline constexpr Month August{8};
inline constexpr Month September{9};
inline constexpr Month October{10};
inline constexpr Month November{11};
inline constexpr Month December{12};

// time.Weekday: Sunday is 0.
struct Weekday {
    int value = 0;

    constexpr Weekday() noexcept = default;
    constexpr explicit Weekday(int v) noexcept : value(v) {}
    constexpr explicit operator int() const noexcept { return value; }
    // "Sunday", or "%!Weekday(7)" out of range, as in Go.
    string String() const;

    friend constexpr auto operator<=>(Weekday a, Weekday b) noexcept = default;
    friend constexpr Weekday operator+(Weekday d, int n) noexcept { return Weekday(d.value + n); }
    friend constexpr Weekday operator-(Weekday d, int n) noexcept { return Weekday(d.value - n); }
};

inline constexpr Weekday Sunday{0};
inline constexpr Weekday Monday{1};
inline constexpr Weekday Tuesday{2};
inline constexpr Weekday Wednesday{3};
inline constexpr Weekday Thursday{4};
inline constexpr Weekday Friday{5};
inline constexpr Weekday Saturday{6};

// time.Location: a time zone, with its transitions. Locations live as long
// as the program, and the ones of one name (LoadLocation) or name and
// offset (FixedZone) are shared, so Times hold them by plain pointer.
class Location {
public:
    // The location's name: "UTC", "Local", "America/New_York", ...
    string String() const;

    constexpr Location() noexcept = default;
    Location(const Location&) = default;
    Location& operator=(const Location&) = default;

    // Go's zone and zoneTrans, for the time package's own code.
    struct zone {
        string name;  // abbreviated name, "CET"
        int offset = 0;  // seconds east of UTC
        bool isDST = false;
    };
    struct zoneTrans {
        int64 when = 0;  // seconds since 1970 UTC
        uint8 index = 0;  // the zone in effect from then on
        bool isstd = false, isutc = false;
    };
    // The result of lookup: the zone in effect at a time, and the times
    // (Unix seconds) it is in effect between.
    struct lookup_result {
        string name;
        int offset = 0;
        int64 start = 0, end = 0;
        bool isDST = false;
    };

    // Go's Location.lookup, lookupName, get and the data, used by the
    // time package's sources.
    lookup_result lookup(int64 sec) const;
    std::pair<int, bool> lookupName(std::string_view name, int64 unix) const;
    const Location* get() const;

    string name;
    std::vector<zone> zones;
    std::vector<zoneTrans> tx;
    string extend;  // the POSIX TZ rule for times after the last transition
    int64 cacheStart = 0, cacheEnd = 0;
    bool hasCache = false;
    zone cacheZone;

private:
    int lookupFirstZone() const;
    bool firstZoneUsed() const;
};

// time.UTC
extern const Location* const UTC;
// time.Local: the system's zone, from TZ (a zone name, a file, or "" for
// UTC) or else /etc/localtime, loaded on first use.
extern const Location* Local;

// time.FixedZone: a Location that is always offset seconds east of UTC.
const Location* FixedZone(std::string_view name, int offset);
// time.LoadLocation: "UTC" (or ""), "Local", or a name in the IANA time
// zone database, read from $ZONEINFO (a directory or zip file) or the
// system's zoneinfo directories.
std::pair<const Location*, error> LoadLocation(std::string_view name);
// time.LoadLocationFromTZData: a Location from TZif data.
std::pair<const Location*, error> LoadLocationFromTZData(std::string_view name, std::span<const byte> data);

// The layouts of Format and Parse, as in Go: the reference time
// Mon Jan 2 15:04:05 MST 2006 written the way the time should look.
inline constexpr std::string_view Layout = "01/02 03:04:05PM '06 -0700";
inline constexpr std::string_view ANSIC = "Mon Jan _2 15:04:05 2006";
inline constexpr std::string_view UnixDate = "Mon Jan _2 15:04:05 MST 2006";
inline constexpr std::string_view RubyDate = "Mon Jan 02 15:04:05 -0700 2006";
inline constexpr std::string_view RFC822 = "02 Jan 06 15:04 MST";
inline constexpr std::string_view RFC822Z = "02 Jan 06 15:04 -0700";
inline constexpr std::string_view RFC850 = "Monday, 02-Jan-06 15:04:05 MST";
inline constexpr std::string_view RFC1123 = "Mon, 02 Jan 2006 15:04:05 MST";
inline constexpr std::string_view RFC1123Z = "Mon, 02 Jan 2006 15:04:05 -0700";
inline constexpr std::string_view RFC3339 = "2006-01-02T15:04:05Z07:00";
inline constexpr std::string_view RFC3339Nano = "2006-01-02T15:04:05.999999999Z07:00";
inline constexpr std::string_view Kitchen = "3:04PM";
inline constexpr std::string_view Stamp = "Jan _2 15:04:05";
inline constexpr std::string_view StampMilli = "Jan _2 15:04:05.000";
inline constexpr std::string_view StampMicro = "Jan _2 15:04:05.000000";
inline constexpr std::string_view StampNano = "Jan _2 15:04:05.000000000";
inline constexpr std::string_view DateTime = "2006-01-02 15:04:05";
inline constexpr std::string_view DateOnly = "2006-01-02";
inline constexpr std::string_view TimeOnly = "15:04:05";

// time.Time: an instant with nanosecond precision, in a Location, with a
// monotonic clock reading when it came from Now. The zero Time is
// January 1, year 1, 00:00:00 UTC.
class Time {
public:
    constexpr Time() noexcept = default;

    // Whether t is the zero time, January 1, year 1, 00:00:00 UTC.
    constexpr bool IsZero() const noexcept { return sec_ == 0 && nsec_ == 0; }

    bool After(const Time& u) const noexcept;
    bool Before(const Time& u) const noexcept;
    // -1, 0 or +1 as t is before, equal to or after u.
    int Compare(const Time& u) const noexcept;
    // Whether t and u are the same instant, whatever their locations.
    bool Equal(const Time& u) const noexcept;

    Time Add(Duration d) const noexcept;
    // t-u, using the monotonic readings when both have them.
    Duration Sub(const Time& u) const noexcept;
    // The time years, months and days later, normalized as Date does.
    Time AddDate(int years, int months, int days) const;

    // The calendar fields, in t's location.
    std::tuple<int, time::Month, int> Date() const;
    int Year() const;
    time::Month Month() const;
    int Day() const;
    time::Weekday Weekday() const;
    std::pair<int, int> ISOWeek() const;
    std::tuple<int, int, int> Clock() const;
    int Hour() const;
    int Minute() const;
    int Second() const;
    int Nanosecond() const noexcept { return nsec_; }
    int YearDay() const;

    Time UTC() const noexcept;
    Time Local() const noexcept;
    Time In(const time::Location* loc) const;
    const time::Location* Location() const noexcept;
    // The zone's abbreviation and its offset in seconds east of UTC.
    std::pair<string, int> Zone() const;
    // When the zone in effect at t starts and ends (zero if unbounded).
    std::pair<Time, Time> ZoneBounds() const;
    bool IsDST() const;

    int64 Unix() const noexcept { return sec_ + internalToUnix; }
    int64 UnixMilli() const noexcept { return Unix() * 1000 + nsec_ / 1000000; }
    int64 UnixMicro() const noexcept { return Unix() * 1000000 + nsec_ / 1000; }
    int64 UnixNano() const noexcept { return Unix() * 1000000000 + nsec_; }

    // The time rounded down, or to the nearest, multiple of d since the
    // zero time. They strip the monotonic reading.
    Time Truncate(Duration d) const;
    Time Round(Duration d) const;

    // t written in layout (see Layout).
    string Format(std::string_view layout) const;
    slice<byte> AppendFormat(slice<byte> b, std::string_view layout) const;
    // "2006-01-02 15:04:05.999999999 -0700 MST", with " m=+0.001" for the
    // monotonic reading.
    string String() const;
    // t as a Go expression: "time.Date(2009, time.November, 10, ...)".
    string GoString() const;

    // RFC 3339 with nanoseconds, as JSON strings and text.
    std::pair<slice<byte>, error> MarshalJSON() const;
    error UnmarshalJSON(std::span<const byte> data);
    std::pair<slice<byte>, error> MarshalText() const;
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const;
    error UnmarshalText(std::span<const byte> data);

    // RFC 1123 in UTC, as HTTP uses: "Mon, 02 Jan 2006 15:04:05 GMT".
    string FormatHTTP() const;
    // RFC 3339 in UTC with nanoseconds.
    string FormatRFC3339() const;

    // time.Unix, also as a member for older go++ code.
    static Time Unix(int64 sec, int64 nsec) noexcept;

    // Seconds from the internal epoch (January 1, year 1) to the Unix one.
    static constexpr int64 unixToInternal = (1969LL * 365 + 1969 / 4 - 1969 / 100 + 1969 / 400) * 86400;
    static constexpr int64 internalToUnix = -unixToInternal;

    // For the time package's sources.
    struct internal;
    friend struct internal;
    friend Time Now() noexcept;

private:
    int64 sec_ = 0;   // seconds since January 1, year 1, 00:00:00 UTC
    int32 nsec_ = 0;        // [0, 999999999]
    bool hasMono_ = false;  // whether mono_ is a monotonic clock reading
    int64 mono_ = 0;        // the monotonic reading
    const time::Location* loc_ = nullptr;  // nullptr is UTC
};

// time.Now: the current local time, with a monotonic reading.
Time Now() noexcept;

// time.Since
Duration Since(const Time& t) noexcept;

// time.Until
Duration Until(const Time& t) noexcept;

// time.Unix: the local Time sec seconds and nsec nanoseconds after
// January 1, 1970 UTC. nsec may be outside [0, 999999999].
Time Unix(int64 sec, int64 nsec) noexcept;
// time.UnixMilli
Time UnixMilli(int64 msec) noexcept;
// time.UnixMicro
Time UnixMicro(int64 usec) noexcept;

// time.Date: the Time in loc for the date and clock, normalizing values
// outside their ranges (October 32 is November 1). In a zone transition
// the result is one of the two possible times. Panics if loc is nullptr.
Time Date(int year, Month month, int day, int hour, int min, int sec, int nsec, const Location* loc);

// time.ParseError
struct ParseError final : error_interface {
    string Layout;
    string Value;
    string LayoutElem;
    string ValueElem;
    string Message;

    ParseError(string layout, string value, string layoutElem, string valueElem, string message)
        : Layout(std::move(layout)), Value(std::move(value)), LayoutElem(std::move(layoutElem)),
          ValueElem(std::move(valueElem)), Message(std::move(message)) {}
    string Error() const override;
};

// time.Parse: value read with layout (see Layout). Without a zone in value
// the result is UTC; a zone offset known to Local gives Local.
std::pair<Time, error> Parse(std::string_view layout, std::string_view value);
// time.ParseInLocation: Parse, with loc in place of UTC and Local.
std::pair<Time, error> ParseInLocation(std::string_view layout, std::string_view value, const Location* loc);

// time.Sleep: parks the fiber for at least d.
void Sleep(Duration d);

// time.Timer: sends the time on c when it fires, or calls f (AfterFunc).
class Timer : public object {
public:
    chan<Time> C;

    // Stops the timer. Returns false if it already fired or was stopped.
    bool Stop() noexcept;
    // Restarts the timer to fire after d. Returns whether it was active.
    bool Reset(Duration d) noexcept;

    ~Timer() override;

private:
    friend ptr<Timer> NewTimer(Duration d);
    friend ptr<Timer> AfterFunc(Duration d, std::function<void()> f);
    friend class Ticker;
    struct impl;
    Timer();
    impl* impl_;
};

// time.NewTimer
ptr<Timer> NewTimer(Duration d);

// time.After: a channel that receives the time after d.
chan<Time> After(Duration d);

// time.AfterFunc: calls f on its own fiber after d.
ptr<Timer> AfterFunc(Duration d, std::function<void()> f);

// time.Ticker: sends the time on c every period.
class Ticker : public object {
public:
    chan<Time> C;
    void Stop() noexcept;
    void Reset(Duration d) noexcept;
    ~Ticker() override;

private:
    friend ptr<Ticker> NewTicker(Duration d);
    Ticker();
    Timer::impl* impl_;
};

// time.NewTicker
ptr<Ticker> NewTicker(Duration d);

}  // namespace gopp::time

template <>
struct std::formatter<gopp::time::Duration> : std::formatter<std::string_view> {
    auto format(const gopp::time::Duration& d, auto& ctx) const {
        return std::formatter<std::string_view>::format(d.String().view(), ctx);
    }
};

template <>
struct std::formatter<gopp::time::Time> : std::formatter<std::string_view> {
    auto format(const gopp::time::Time& t, auto& ctx) const {
        return std::formatter<std::string_view>::format(t.String().view(), ctx);
    }
};

template <>
struct std::formatter<gopp::time::Month> : std::formatter<std::string_view> {
    auto format(const gopp::time::Month& m, auto& ctx) const {
        return std::formatter<std::string_view>::format(m.String().view(), ctx);
    }
};

template <>
struct std::formatter<gopp::time::Weekday> : std::formatter<std::string_view> {
    auto format(const gopp::time::Weekday& d, auto& ctx) const {
        return std::formatter<std::string_view>::format(d.String().view(), ctx);
    }
};
