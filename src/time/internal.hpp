// The time package's internals, shared by its sources and its tests: Go's
// unexported Time methods and constants.
#pragma once

#include <gopp/time.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace gopp::time {

inline constexpr int64 secondsPerMinute = 60;
inline constexpr int64 secondsPerHour = 60 * secondsPerMinute;
inline constexpr int64 secondsPerDay = 24 * secondsPerHour;
inline constexpr int64 secondsPerWeek = 7 * secondsPerDay;
inline constexpr int64 daysPer400Years = 365 * 400 + 97;
inline constexpr int64 marchThruDecember = 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30 + 31;

// The absolute epoch: March 1 of a year far before year 1, so that all
// times of interest are positive and years start after February.
inline constexpr int64 absoluteYears = 292277022400;
inline constexpr int64 absoluteToInternal =
    -((absoluteYears * 365 + absoluteYears / 400 * 97 + marchThruDecember) * secondsPerDay);
inline constexpr int64 internalToAbsolute = -absoluteToInternal;
inline constexpr int64 unixToInternal = Time::unixToInternal;
inline constexpr int64 internalToUnix = Time::internalToUnix;
inline constexpr int64 absoluteToUnix = absoluteToInternal + internalToUnix;
inline constexpr int64 unixToAbsolute = unixToInternal + internalToAbsolute;

// The wall times a monotonic reading may go with: years 1885 to 2157.
inline constexpr int64 wallToInternal = (1884LL * 365 + 1884 / 4 - 1884 / 100 + 1884 / 400) * secondsPerDay;
inline constexpr int64 minWall = wallToInternal;
inline constexpr int64 maxWall = wallToInternal + ((int64(1) << 33) - 1);

inline constexpr int64 alpha = INT64_MIN;  // math.MinInt64: the start of time
inline constexpr int64 omega = INT64_MAX;  // math.MaxInt64: the end of time

// Go's absSeconds and absDays: seconds and days since the absolute epoch.
using absSeconds = uint64;
using absDays = uint64;

absDays dateToAbsDays(int64 year, Month month, int day);
std::tuple<int, Month, int> absDate(absDays days);
std::pair<int, int> absYearYday(absDays days);
Weekday absWeekday(absDays days);
std::tuple<int, int, int> absClock(absSeconds abs);

bool isLeap(int year);
int daysBefore(Month m);
int daysIn(Month m, int year);

extern const char* const longDayNames[7];
extern const char* const shortDayNames[7];
extern const char* const shortMonthNames[12];
extern const char* const longMonthNames[12];

// Go's quote, as ParseError and GoString use it.
std::string quote(std::string_view s);
// Go's appendInt: x in decimal, zero-padded to width.
void appendInt(std::string& b, int64 x, int width);

// Go's unexported Time methods.
struct Time::internal {
    static int64 sec(const Time& t) noexcept { return t.sec_; }
    static int32 nsec(const Time& t) noexcept { return t.nsec_; }
    // Go's mono: the reading, or 0 without one.
    static int64 mono(const Time& t) noexcept { return t.hasMono_ ? t.mono_ : 0; }
    static bool hasMono(const Time& t) noexcept { return t.hasMono_; }
    static int64 unixSec(const Time& t) noexcept { return t.sec_ + internalToUnix; }
    static const time::Location* loc(const Time& t) noexcept { return t.loc_; }
    // Go's unixTime: a Local Time.
    static Time unixTime(int64 sec, int32 nsec) noexcept {
        Time t;
        t.sec_ = sec + unixToInternal;
        t.nsec_ = nsec;
        t.loc_ = time::Local;
        return t;
    }
    static Time make(int64 sec, int32 nsec, const time::Location* loc) noexcept {
        Time t;
        t.sec_ = sec;
        t.nsec_ = nsec;
        t.loc_ = loc;
        return t;
    }
    static void addSec(Time& t, int64 d) noexcept;
    static void setLoc(Time& t, const time::Location* loc) noexcept;
    static void stripMono(Time& t) noexcept {
        t.hasMono_ = false;
        t.mono_ = 0;
    }
    // Go's setMono: only for wall times a reading may go with.
    static void setMono(Time& t, int64 m) noexcept {
        if (!t.hasMono_ && (t.sec_ < minWall || maxWall < t.sec_)) return;
        t.hasMono_ = true;
        t.mono_ = m;
    }
    // Seconds since the absolute epoch in t's location.
    static absSeconds absSec(const Time& t);
    // The zone name, its offset and absSec, in one lookup.
    static std::tuple<string, int, absSeconds> locabs(const Time& t);
    // Go's appendFormat, appendFormatRFC3339 and appendStrictRFC3339.
    static void appendFormat(const Time& t, std::string& b, std::string_view layout);
    static void appendFormatRFC3339(const Time& t, std::string& b, bool nanos);
    static error appendStrictRFC3339(const Time& t, std::string& b);
};

// Go's parseStrictRFC3339, for UnmarshalJSON and UnmarshalText.
std::pair<Time, error> parseStrictRFC3339(std::string_view b);

// Test hooks, as Go's export_test.go has them.
namespace testing_hooks {
// Go's ForceUSPacificForTesting: Local becomes America/Los_Angeles.
void ForceUSPacificForTesting();
// Go's ResetLocalOnceForTest: Local is loaded again on next use.
void ResetLocalOnceForTest();
// Go's ForceZipFileForTesting: LoadLocation reads only the zip file zip
// (or the system directories again with "").
void ForceZipFileForTesting(std::string_view zip);
// Go's Tzset, TzsetName, TzsetOffset and TzsetRule.
struct TzsetResult {
    std::string name;
    int offset = 0;
    int64 start = 0, end = 0;
    bool isDST = false, ok = false;
};
TzsetResult Tzset(std::string_view s, int64 lastTxSec, int64 sec);
std::tuple<std::string, std::string, bool> TzsetName(std::string_view s);
std::tuple<int, std::string, bool> TzsetOffset(std::string_view s);
struct Rule {
    int Kind = 0, Day = 0, Week = 0, Mon = 0, Time = 0;
    bool operator==(const Rule&) const = default;
};
inline constexpr int RuleJulian = 0, RuleDOY = 1, RuleMonthWeekDay = 2;
std::tuple<Rule, std::string, bool> TzsetRule(std::string_view s);
// Go's NextStdChunk and StdChunkNames.
std::tuple<std::string, int, std::string> NextStdChunk(std::string_view layout);
// Go's ParseTimeZone.
std::pair<int, bool> ParseTimeZone(std::string_view value);
// Go's ErrLocation, the error of an invalid location name.
extern const error ErrLocation;
// Go's MinMonoTime, MaxMonoTime and NotMonoNegativeTime.
inline Time MinMonoTime() {
    Time t = Time::internal::make(wallToInternal, 0, UTC);
    Time::internal::setMono(t, INT64_MIN);
    return t;
}
inline Time MaxMonoTime() {
    Time t = Time::internal::make(wallToInternal, 0, UTC);
    Time::internal::setMono(t, INT64_MAX);
    return t;
}
inline Time NotMonoNegativeTime() { return Time::internal::make(INT64_MIN + 50, 0, nullptr); }
// Go's ReadFile: a zone file read as zoneinfo reads them.
std::pair<std::string, error> ReadFile(const std::string& name);
}  // namespace testing_hooks

}  // namespace gopp::time
