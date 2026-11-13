// Ported from Go 1.27.3's time/time_test.go and time/mono_test.go, with
// Go's test names.
//
// As in Go, Local is America/Los_Angeles for these tests (from the
// system's zoneinfo).
//
// Left out: the gob and binary marshaling tests (go++ has no
// MarshalBinary), TestCountMallocs and TestUnmarshalTextAllocations
// (allocation counts), and the timer tests, which sync_time_test.cpp
// covers. testing/quick is a fixed-seed random sample here, and
// math/big in TestTruncateRound is 128-bit arithmetic. Tests that Go runs
// with the platform zoneinfo disabled (to use its own zip file) use the
// system's zoneinfo.
#include "go_helper.hpp"
#include "time/internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::time;
namespace hooks = gopp::time::testing_hooks;

namespace {

class GoTime : public ::testing::Test {
protected:
    static void SetUpTestSuite() { hooks::ForceUSPacificForTesting(); }
    static void TearDownTestSuite() { hooks::ResetLocalOnceForTest(); }
};

std::string S(const string& s) { return std::string(s.view()); }
std::string S(const error& e) { return e ? std::string(e.Error().view()) : "<nil>"; }

// Go's t1 == t2 on Times: the same instant, location and reading.
bool identical(const Time& a, const Time& b) {
    using I = Time::internal;
    return I::sec(a) == I::sec(b) && I::nsec(a) == I::nsec(b) && I::hasMono(a) == I::hasMono(b) &&
           I::mono(a) == I::mono(b) && I::loc(a) == I::loc(b);
}

constexpr Duration minDuration{INT64_MIN};
constexpr Duration maxDuration{INT64_MAX};

}  // namespace

TEST_F(GoTime, ZeroTime) {
    Time zero;
    auto [year, month, day] = zero.Date();
    auto [hour, min, sec] = zero.Clock();
    EXPECT_TRUE(year == 1 && month == January && day == 1 && hour == 0 && min == 0 && sec == 0 &&
                zero.Nanosecond() == 0 && zero.YearDay() == 1 && zero.Weekday() == Monday)
        << "zero time = " << zero.Weekday().String().view() << " " << month.String().view() << " " << day << " year "
        << year;
}

// We should be in PST/PDT, but if the time zone files are missing we won't
// be. The purpose of this test is to at least explain why some of the
// subsequent tests fail.
TEST_F(GoTime, ZoneData) {
    Time lt = Now();
    // PST is 8 hours west, PDT is 7 hours west. We could use the name but
    // it's not unique.
    auto [name, off] = lt.Zone();
    EXPECT_TRUE(off == -8 * 60 * 60 || off == -7 * 60 * 60)
        << "Unable to find US Pacific time zone data for testing; time zone is " << name.view() << " offset " << off;
}

namespace {

// parsedTime is the struct representing a parsed time value.
struct parsedTime {
    int Year;
    time::Month Month;
    int Day;
    int Hour, Minute, Second;  // 15:04:05 is 15, 4, 5.
    int Nanosecond;            // Fractional second.
    time::Weekday Weekday;
    int ZoneOffset;    // seconds east of UTC, e.g. -7*60*60 for -0700
    std::string Zone;  // e.g., "MST"
};

struct TimeTest {
    int64 seconds;
    parsedTime golden;
};

const std::vector<TimeTest> utctests = {
    {0, {1970, January, 1, 0, 0, 0, 0, Thursday, 0, "UTC"}},
    {1221681866, {2008, September, 17, 20, 4, 26, 0, Wednesday, 0, "UTC"}},
    {-1221681866, {1931, April, 16, 3, 55, 34, 0, Thursday, 0, "UTC"}},
    {-11644473600, {1601, January, 1, 0, 0, 0, 0, Monday, 0, "UTC"}},
    {599529660, {1988, December, 31, 0, 1, 0, 0, Saturday, 0, "UTC"}},
    {978220860, {2000, December, 31, 0, 1, 0, 0, Sunday, 0, "UTC"}},
};

const std::vector<TimeTest> nanoutctests = {
    {0, {1970, January, 1, 0, 0, 0, 100000000, Thursday, 0, "UTC"}},
    {1221681866, {2008, September, 17, 20, 4, 26, 200000000, Wednesday, 0, "UTC"}},
};

const std::vector<TimeTest> localtests = {
    {0, {1969, December, 31, 16, 0, 0, 0, Wednesday, -8 * 60 * 60, "PST"}},
    {1221681866, {2008, September, 17, 13, 4, 26, 0, Wednesday, -7 * 60 * 60, "PDT"}},
    {2159200800, {2038, June, 3, 11, 0, 0, 0, Thursday, -7 * 60 * 60, "PDT"}},
    {2152173599, {2038, March, 14, 1, 59, 59, 0, Sunday, -8 * 60 * 60, "PST"}},
    {2152173600, {2038, March, 14, 3, 0, 0, 0, Sunday, -7 * 60 * 60, "PDT"}},
    {2152173601, {2038, March, 14, 3, 0, 1, 0, Sunday, -7 * 60 * 60, "PDT"}},
    {2172733199, {2038, November, 7, 1, 59, 59, 0, Sunday, -7 * 60 * 60, "PDT"}},
    {2172733200, {2038, November, 7, 1, 0, 0, 0, Sunday, -8 * 60 * 60, "PST"}},
    {2172733201, {2038, November, 7, 1, 0, 1, 0, Sunday, -8 * 60 * 60, "PST"}},
};

const std::vector<TimeTest> nanolocaltests = {
    {0, {1969, December, 31, 16, 0, 0, 100000000, Wednesday, -8 * 60 * 60, "PST"}},
    {1221681866, {2008, September, 17, 13, 4, 26, 300000000, Wednesday, -7 * 60 * 60, "PDT"}},
};

bool same(const Time& t, const parsedTime& u) {
    // Check aggregates.
    auto [year, month, day] = t.Date();
    auto [hour, min, sec] = t.Clock();
    auto [name, offset] = t.Zone();
    if (year != u.Year || month != u.Month || day != u.Day || hour != u.Hour || min != u.Minute || sec != u.Second ||
        name.view() != u.Zone || offset != u.ZoneOffset) {
        return false;
    }
    // Check individual entries.
    return t.Year() == u.Year && t.Month() == u.Month && t.Day() == u.Day && t.Hour() == u.Hour &&
           t.Minute() == u.Minute && t.Second() == u.Second && t.Nanosecond() == u.Nanosecond &&
           t.Weekday() == u.Weekday;
}

std::string have(const Time& t) { return S(t.Format(std::string(RFC3339) + " MST")); }

}  // namespace

TEST_F(GoTime, UnixUTC) {
    for (const auto& test : utctests) {
        Time tm = Unix(test.seconds, 0).UTC();
        EXPECT_EQ(tm.Unix(), test.seconds);
        EXPECT_TRUE(same(tm, test.golden)) << "Unix(" << test.seconds << ", 0): have " << have(tm);
    }
}

TEST_F(GoTime, UnixNanoUTC) {
    for (const auto& test : nanoutctests) {
        int64 nsec = test.seconds * 1000000000 + test.golden.Nanosecond;
        Time tm = Unix(0, nsec).UTC();
        EXPECT_EQ(tm.Unix() * 1000000000 + tm.Nanosecond(), nsec);
        EXPECT_TRUE(same(tm, test.golden)) << "Unix(0, " << nsec << "): have " << have(tm);
    }
}

TEST_F(GoTime, Unix) {
    for (const auto& test : localtests) {
        Time tm = Unix(test.seconds, 0);
        EXPECT_EQ(tm.Unix(), test.seconds);
        EXPECT_TRUE(same(tm, test.golden)) << "Unix(" << test.seconds << ", 0): have " << have(tm);
    }
}

TEST_F(GoTime, UnixNano) {
    for (const auto& test : nanolocaltests) {
        int64 nsec = test.seconds * 1000000000 + test.golden.Nanosecond;
        Time tm = Unix(0, nsec);
        EXPECT_EQ(tm.Unix() * 1000000000 + tm.Nanosecond(), nsec);
        EXPECT_TRUE(same(tm, test.golden)) << "Unix(0, " << nsec << "): have " << have(tm);
    }
}

TEST_F(GoTime, UnixUTCAndBack) {
    std::mt19937_64 rng(2);
    // Try a reasonable date first, then the huge ones.
    for (int i = 0; i < 10000; i++) {
        int64 sec = static_cast<int32>(rng());
        ASSERT_EQ(Unix(sec, 0).UTC().Unix(), sec);
    }
    for (int i = 0; i < 10000; i++) {
        int64 sec = static_cast<int64>(rng());
        ASSERT_EQ(Unix(sec, 0).UTC().Unix(), sec);
    }
}

TEST_F(GoTime, UnixNanoUTCAndBack) {
    std::mt19937_64 rng(3);
    auto f = [](int64 nsec) {
        Time t = Unix(0, nsec).UTC();
        return t.Unix() * 1000000000 + t.Nanosecond() == nsec;
    };
    for (int i = 0; i < 10000; i++) ASSERT_TRUE(f(static_cast<int32>(rng())));
    for (int i = 0; i < 10000; i++) ASSERT_TRUE(f(static_cast<int64>(rng())));
}

TEST_F(GoTime, UnixMilli) {
    std::mt19937_64 rng(4);
    for (int i = 0; i < 10000; i++) {
        int64 msec = static_cast<int64>(rng());
        ASSERT_EQ(UnixMilli(msec).UnixMilli(), msec);
    }
}

TEST_F(GoTime, UnixMicro) {
    std::mt19937_64 rng(5);
    for (int i = 0; i < 10000; i++) {
        int64 usec = static_cast<int64>(rng());
        ASSERT_EQ(UnixMicro(usec).UnixMicro(), usec);
    }
}

namespace {

// The time routines provide no way to get absolute time (seconds since
// zero), but we need it to compute the right answer for bizarre roundings
// like "to the nearest 3 ns". Compute as t - year1 = (t - 1970) + (1970 -
// 2001) + (2001 - 1). t - 1970 is returned by Unix and Nanosecond. 1970 -
// 2001 is -(31*365+8)*86400 = -978307200 seconds. 2001 - 1 is
// 2000*365.2425*86400 = 63113904000 seconds.
constexpr int64 unixToZero = -978307200 + 63113904000;

}  // namespace

TEST_F(GoTime, TruncateRound) {
    __extension__ typedef __int128 int128;
    auto testOne = [](int64 ti, int64 tns, int64 di) {
        Time t0 = Unix(ti, tns).UTC();
        Duration d(di);
        if (d.ns < 0) d = Duration(-d.ns);
        if (d.ns <= 0) d = Duration(1);

        // Compute bt = absolute nanoseconds.
        int128 bt = static_cast<int128>(t0.Unix() + unixToZero) * 1000000000 + t0.Nanosecond();
        // Compute quotient and remainder mod d (floored, as big.Int's
        // DivMod is Euclidean).
        int128 br = bt % d.ns;
        if (br < 0) br += d.ns;

        // To truncate, subtract remainder. br is < d, so it fits in an
        // int64.
        int64 r = static_cast<int64>(br);
        Time t1 = t0.Add(Duration(-r));

        // Check that time.Truncate works.
        Time trunc = t0.Truncate(d);
        if (!identical(trunc, t1)) {
            ADD_FAILURE() << "Time.Truncate(" << S(t0.Format(RFC3339Nano)) << ", " << d.ns << ") = " << S(trunc.String())
                          << ", want " << S(t1.Format(RFC3339Nano));
            return false;
        }

        // To round, add d back if remainder r > d/2 or r == exactly d/2.
        if (r > d.ns / 2 || r + r == d.ns) t1 = t1.Add(d);

        // Check that time.Round works.
        Time rnd = t0.Round(d);
        if (!identical(rnd, t1)) {
            ADD_FAILURE() << "Time.Round(" << S(t0.Format(RFC3339Nano)) << ", " << d.ns << ") = " << S(rnd.String())
                          << ", want " << S(t1.Format(RFC3339Nano));
            return false;
        }
        return true;
    };

    // manual test cases
    struct {
        Time t;
        Duration d;
    } truncateRoundTests[] = {
        {Date(-1, January, 1, 12, 15, 30, 500000000, UTC), Duration(3)},
        {Date(-1, January, 1, 12, 15, 31, 500000000, UTC), Duration(3)},
        {Date(2012, January, 1, 12, 15, 30, 500000000, UTC), Second},
        {Date(2012, January, 1, 12, 15, 31, 500000000, UTC), Second},
        {Unix(-19012425939, 649146258), Duration(7435029458905025217)},  // 5.8*d rounds to 6*d, but .8*d+.8*d < 0 < d
    };
    for (const auto& tt : truncateRoundTests) testOne(tt.t.Unix(), tt.t.Nanosecond(), tt.d.ns);

    // exhaustive near 0
    for (int i = 0; i < 100; i++) {
        for (int j = 1; j < 100; j++) {
            if (!testOne(unixToZero, i, j) || !testOne(unixToZero, -i, j)) return;
        }
    }

    // randomly generated test cases
    std::mt19937_64 rng(6);
    constexpr int maxCount = 10000;

    // divisors of Second
    for (int n = 0; n < maxCount; n++) {
        int64 ti = static_cast<int64>(rng());
        int32 tns = static_cast<int32>(rng());
        int32 logdi = static_cast<int32>(rng());
        int64 d = 1;
        int a = logdi % 9;  // Go's uint(a) shifts a negative a out to 0
        int b = (logdi >> 16) % 9;
        d = a < 0 ? 0 : d << a;
        for (int i = 0; i < b; i++) d *= 5;
        // Make room for unix ↔ internal conversion. We don't care about
        // behavior too close to ± 2^63 Unix seconds.
        ti >>= 1;
        if (!testOne(ti, tns, d)) return;
    }

    // multiples of Second
    for (int n = 0; n < maxCount; n++) {
        int64 ti = static_cast<int64>(rng()) >> 1;  // see comment above
        int32 tns = static_cast<int32>(rng());
        int64 d = static_cast<int64>(static_cast<int32>(rng())) * Second.ns;
        if (d < 0) d = -d;
        if (!testOne(ti, tns, d)) return;
    }

    // halfway cases
    for (int n = 0; n < maxCount; n++) {
        int64 tns = static_cast<int64>(rng());
        int64 di = static_cast<int64>(rng());
        di &= 0xfffffffe;
        if (di == 0) di = 2;
        tns -= tns % di;
        if (tns < 0) tns += di / 2;
        else tns -= di / 2;
        if (!testOne(0, tns, di)) return;
    }

    // full generality
    for (int n = 0; n < maxCount; n++) {
        int64 ti = static_cast<int64>(rng()) >> 1;  // see comment above
        int32 tns = static_cast<int32>(rng());
        int64 di = static_cast<int64>(rng());
        if (!testOne(ti, tns, di)) return;
    }
}

TEST_F(GoTime, ISOWeek) {
    struct {
        int year;        // year
        int month, day;  // month and day
        int yex;         // expected year
        int wex;         // expected week
    } isoWeekTests[] = {
        {1981, 1, 1, 1981, 1},    {1982, 1, 1, 1981, 53},   {1983, 1, 1, 1982, 52},   {1984, 1, 1, 1983, 52},
        {1985, 1, 1, 1985, 1},    {1986, 1, 1, 1986, 1},    {1987, 1, 1, 1987, 1},    {1988, 1, 1, 1987, 53},
        {1989, 1, 1, 1988, 52},   {1990, 1, 1, 1990, 1},    {1991, 1, 1, 1991, 1},    {1992, 1, 1, 1992, 1},
        {1993, 1, 1, 1992, 53},   {1994, 1, 1, 1993, 52},   {1995, 1, 2, 1995, 1},    {1996, 1, 1, 1996, 1},
        {1996, 1, 7, 1996, 1},    {1996, 1, 8, 1996, 2},    {1997, 1, 1, 1997, 1},    {1998, 1, 1, 1998, 1},
        {1999, 1, 1, 1998, 53},   {2000, 1, 1, 1999, 52},   {2001, 1, 1, 2001, 1},    {2002, 1, 1, 2002, 1},
        {2003, 1, 1, 2003, 1},    {2004, 1, 1, 2004, 1},    {2005, 1, 1, 2004, 53},   {2006, 1, 1, 2005, 52},
        {2007, 1, 1, 2007, 1},    {2008, 1, 1, 2008, 1},    {2009, 1, 1, 2009, 1},    {2010, 1, 1, 2009, 53},
        {2010, 1, 1, 2009, 53},   {2011, 1, 1, 2010, 52},   {2011, 1, 2, 2010, 52},   {2011, 1, 3, 2011, 1},
        {2011, 1, 4, 2011, 1},    {2011, 1, 5, 2011, 1},    {2011, 1, 6, 2011, 1},    {2011, 1, 7, 2011, 1},
        {2011, 1, 8, 2011, 1},    {2011, 1, 9, 2011, 1},    {2011, 1, 10, 2011, 2},   {2011, 1, 11, 2011, 2},
        {2011, 6, 12, 2011, 23},  {2011, 6, 13, 2011, 24},  {2011, 12, 25, 2011, 51}, {2011, 12, 26, 2011, 52},
        {2011, 12, 27, 2011, 52}, {2011, 12, 28, 2011, 52}, {2011, 12, 29, 2011, 52}, {2011, 12, 30, 2011, 52},
        {2011, 12, 31, 2011, 52}, {1995, 1, 1, 1994, 52},   {2012, 1, 1, 2011, 52},   {2012, 1, 2, 2012, 1},
        {2012, 1, 8, 2012, 1},    {2012, 1, 9, 2012, 2},    {2012, 12, 23, 2012, 51}, {2012, 12, 24, 2012, 52},
        {2012, 12, 30, 2012, 52}, {2012, 12, 31, 2013, 1},  {2013, 1, 1, 2013, 1},    {2013, 1, 6, 2013, 1},
        {2013, 1, 7, 2013, 2},    {2013, 12, 22, 2013, 51}, {2013, 12, 23, 2013, 52}, {2013, 12, 29, 2013, 52},
        {2013, 12, 30, 2014, 1},  {2014, 1, 1, 2014, 1},    {2014, 1, 5, 2014, 1},    {2014, 1, 6, 2014, 2},
        {2015, 1, 1, 2015, 1},    {2016, 1, 1, 2015, 53},   {2017, 1, 1, 2016, 52},   {2018, 1, 1, 2018, 1},
        {2019, 1, 1, 2019, 1},    {2020, 1, 1, 2020, 1},    {2021, 1, 1, 2020, 53},   {2022, 1, 1, 2021, 52},
        {2023, 1, 1, 2022, 52},   {2024, 1, 1, 2024, 1},    {2025, 1, 1, 2025, 1},    {2026, 1, 1, 2026, 1},
        {2027, 1, 1, 2026, 53},   {2028, 1, 1, 2027, 52},   {2029, 1, 1, 2029, 1},    {2030, 1, 1, 2030, 1},
        {2031, 1, 1, 2031, 1},    {2032, 1, 1, 2032, 1},    {2033, 1, 1, 2032, 53},   {2034, 1, 1, 2033, 52},
        {2035, 1, 1, 2035, 1},    {2036, 1, 1, 2036, 1},    {2037, 1, 1, 2037, 1},    {2038, 1, 1, 2037, 53},
        {2039, 1, 1, 2038, 52},   {2040, 1, 1, 2039, 52},
    };
    // Selected dates and corner cases
    for (const auto& wt : isoWeekTests) {
        Time dt = Date(wt.year, Month(wt.month), wt.day, 0, 0, 0, 0, UTC);
        auto [y, w] = dt.ISOWeek();
        EXPECT_TRUE(w == wt.wex && y == wt.yex)
            << "got " << y << "/" << w << "; expected " << wt.yex << "/" << wt.wex << " for " << wt.year << "-"
            << wt.month << "-" << wt.day;
    }
    // The only real invariant: Jan 04 is in week 1
    for (int year = 1950; year < 2100; year++) {
        auto [y, w] = Date(year, January, 4, 0, 0, 0, 0, UTC).ISOWeek();
        EXPECT_TRUE(y == year && w == 1) << "got " << y << "/" << w << "; expected " << year << "/1 for Jan 04";
    }
}

TEST_F(GoTime, YearDay) {
    struct {
        int year, month, day;
        int yday;
    } yearDayTests[] = {
        // Non-leap-year tests
        {2007, 1, 1, 1}, {2007, 1, 15, 15}, {2007, 2, 1, 32}, {2007, 2, 15, 46},
        {2007, 3, 1, 60}, {2007, 3, 15, 74}, {2007, 4, 1, 91}, {2007, 12, 31, 365},
        // Leap-year tests
        {2008, 1, 1, 1}, {2008, 1, 15, 15}, {2008, 2, 1, 32}, {2008, 2, 15, 46},
        {2008, 3, 1, 61}, {2008, 3, 15, 75}, {2008, 4, 1, 92}, {2008, 12, 31, 366},
        // Looks like leap-year (but isn't) tests
        {1900, 1, 1, 1}, {1900, 1, 15, 15}, {1900, 2, 1, 32}, {1900, 2, 15, 46},
        {1900, 3, 1, 60}, {1900, 3, 15, 74}, {1900, 4, 1, 91}, {1900, 12, 31, 365},
        // Year one tests (non-leap)
        {1, 1, 1, 1}, {1, 1, 15, 15}, {1, 2, 1, 32}, {1, 2, 15, 46},
        {1, 3, 1, 60}, {1, 3, 15, 74}, {1, 4, 1, 91}, {1, 12, 31, 365},
        // Year minus one tests (non-leap)
        {-1, 1, 1, 1}, {-1, 1, 15, 15}, {-1, 2, 1, 32}, {-1, 2, 15, 46},
        {-1, 3, 1, 60}, {-1, 3, 15, 74}, {-1, 4, 1, 91}, {-1, 12, 31, 365},
        // 400 BC tests (leap-year)
        {-400, 1, 1, 1}, {-400, 1, 15, 15}, {-400, 2, 1, 32}, {-400, 2, 15, 46},
        {-400, 3, 1, 61}, {-400, 3, 15, 75}, {-400, 4, 1, 92}, {-400, 12, 31, 366},
        // Special Cases
        // Gregorian calendar change (no effect)
        {1582, 10, 4, 277}, {1582, 10, 15, 288},
    };
    // Check to see if YearDay is location sensitive
    const Location* yearDayLocations[] = {
        FixedZone("UTC-8", -8 * 60 * 60), FixedZone("UTC-4", -4 * 60 * 60), UTC,
        FixedZone("UTC+4", 4 * 60 * 60),  FixedZone("UTC+8", 8 * 60 * 60),
    };
    for (int i = 0; i < 5; i++) {
        const Location* loc = yearDayLocations[i];
        for (const auto& ydt : yearDayTests) {
            Time dt = Date(ydt.year, Month(ydt.month), ydt.day, 0, 0, 0, 0, loc);
            int yday = dt.YearDay();
            if (yday != ydt.yday) {
                ADD_FAILURE() << "Date(" << ydt.year << "-" << ydt.month << "-" << ydt.day << " in "
                              << loc->String().view() << ").YearDay() = " << yday << ", want " << ydt.yday;
                continue;
            }
            if (ydt.year < 0 || ydt.year > 9999) continue;
            int off = (i - 2) * 4;
            std::string f = std::format("{:04d}-{:02d}-{:02d} {:03d} {}{:02d}00", ydt.year, ydt.month, ydt.day, ydt.yday,
                                        off < 0 ? '-' : '+', off < 0 ? -off : off);
            auto [dt1, err] = Parse("2006-01-02 002 -0700", f);
            if (err) {
                ADD_FAILURE() << "Parse(\"2006-01-02 002 -0700\", " << f << "): " << S(err);
                continue;
            }
            EXPECT_TRUE(dt1.Equal(dt)) << f << " = " << S(dt1.String()) << ", want " << S(dt.String());
        }
    }
}

TEST_F(GoTime, DurationString) {
    struct {
        std::string str;
        Duration d;
    } durationTests[] = {
        {"0s", Duration(0)},
        {"1ns", 1 * Nanosecond},
        {"1.1µs", 1100 * Nanosecond},
        {"2.2ms", 2200 * Microsecond},
        {"3.3s", 3300 * Millisecond},
        {"4m5s", 4 * Minute + 5 * Second},
        {"4m5.001s", 4 * Minute + 5001 * Millisecond},
        {"5h6m7.001s", 5 * Hour + 6 * Minute + 7001 * Millisecond},
        {"8m0.000000001s", 8 * Minute + 1 * Nanosecond},
        {"2562047h47m16.854775807s", maxDuration},
        {"-2562047h47m16.854775808s", minDuration},
    };
    for (const auto& tt : durationTests) {
        EXPECT_EQ(S(tt.d.String()), tt.str);
        if (tt.d.ns > 0) { EXPECT_EQ(S((-tt.d).String()), "-" + tt.str); }
    }
}

TEST_F(GoTime, Date) {
    struct {
        int year, month, day, hour, min, sec, nsec;
        const Location* z;
        int64 unix;
    } dateTests[] = {
        {2011, 11, 6, 1, 0, 0, 0, Local, 1320566400},    // 1:00:00 PDT
        {2011, 11, 6, 1, 59, 59, 0, Local, 1320569999},  // 1:59:59 PDT
        {2011, 11, 6, 2, 0, 0, 0, Local, 1320573600},    // 2:00:00 PST

        {2011, 3, 13, 1, 0, 0, 0, Local, 1300006800},    // 1:00:00 PST
        {2011, 3, 13, 1, 59, 59, 0, Local, 1300010399},  // 1:59:59 PST
        {2011, 3, 13, 3, 0, 0, 0, Local, 1300010400},    // 3:00:00 PDT
        {2011, 3, 13, 2, 30, 0, 0, Local, 1300008600},   // 2:30:00 PDT ≡ 1:30 PST
        {2012, 12, 24, 0, 0, 0, 0, Local, 1356336000},   // Leap year

        // Many names for Fri Nov 18 7:56:35 PST 2011
        {2011, 11, 18, 7, 56, 35, 0, Local, 1321631795},                    // Nov 18 7:56:35
        {2011, 11, 19, -17, 56, 35, 0, Local, 1321631795},                  // Nov 19 -17:56:35
        {2011, 11, 17, 31, 56, 35, 0, Local, 1321631795},                   // Nov 17 31:56:35
        {2011, 11, 18, 6, 116, 35, 0, Local, 1321631795},                   // Nov 18 6:116:35
        {2011, 10, 49, 7, 56, 35, 0, Local, 1321631795},                    // Oct 49 7:56:35
        {2011, 11, 18, 7, 55, 95, 0, Local, 1321631795},                    // Nov 18 7:55:95
        {2011, 11, 18, 7, 56, 34, 1000000000, Local, 1321631795},           // Nov 18 7:56:34 + 10⁹ns
        {2011, 12, -12, 7, 56, 35, 0, Local, 1321631795},                   // Dec -21 7:56:35
        {2012, 1, -43, 7, 56, 35, 0, Local, 1321631795},                    // Jan -52 7:56:35 2012
        {2012, January.value - 2, 18, 7, 56, 35, 0, Local, 1321631795},     // (Jan-2) 18 7:56:35 2012
        {2010, December.value + 11, 18, 7, 56, 35, 0, Local, 1321631795},   // (Dec+11) 18 7:56:35 2010
        {1970, 1, 15297, 7, 56, 35, 0, Local, 1321631795},                  // large number of days

        {1970, 1, -25508, 0, 0, 0, 0, Local, -2203948800},  // negative Unix time
    };
    for (const auto& tt : dateTests) {
        Time t = Date(tt.year, Month(tt.month), tt.day, tt.hour, tt.min, tt.sec, tt.nsec, tt.z);
        Time want = Unix(tt.unix, 0);
        EXPECT_TRUE(t.Equal(want)) << "Date(" << tt.year << ", " << tt.month << ", " << tt.day << ", " << tt.hour
                                   << ", " << tt.min << ", " << tt.sec << ", " << tt.nsec << ") = " << S(t.String())
                                   << ", want " << S(want.String());
    }
}

// Several ways of getting from Fri Nov 18 7:56:35 PST 2011 to Thu Mar 19
// 7:56:35 PST 2016
TEST_F(GoTime, AddDate) {
    struct {
        int years, months, days;
    } addDateTests[] = {
        {4, 4, 1},
        {3, 16, 1},
        {3, 15, 30},
        {5, -6, -18 - 30 - 12},
    };
    Time t0 = Date(2011, Month(11), 18, 7, 56, 35, 0, UTC);
    Time t1 = Date(2016, Month(3), 19, 7, 56, 35, 0, UTC);
    for (const auto& at : addDateTests) {
        Time t = t0.AddDate(at.years, at.months, at.days);
        EXPECT_TRUE(t.Equal(t1)) << "AddDate(" << at.years << ", " << at.months << ", " << at.days
                                 << ") = " << S(t.String()) << ", want " << S(t1.String());
    }

    Time t2 = Date(1899, Month(12), 31, 0, 0, 0, 0, UTC);
    int64 days = t2.Unix() / (24 * 60 * 60);
    Time t3 = Unix(0, 0).AddDate(0, 0, static_cast<int>(days));
    EXPECT_TRUE(t2.Equal(t3)) << "AddDate(0, 0, " << days << ") = " << S(t3.String()) << ", want " << S(t2.String());
}

TEST_F(GoTime, DaysIn) {
    struct {
        int year, month, di;
    } daysInTests[] = {
        {2011, 1, 31},   // January, first month, 31 days
        {2011, 2, 28},   // February, non-leap year, 28 days
        {2012, 2, 29},   // February, leap year, 29 days
        {2011, 6, 30},   // June, 30 days
        {2011, 12, 31},  // December, last month, 31 days
    };
    for (const auto& tt : daysInTests) {
        EXPECT_EQ(daysIn(Month(tt.month), tt.year), tt.di) << tt.year << "-" << tt.month;
    }
}

TEST_F(GoTime, AddToExactSecond) {
    // Add an amount to the current time to round it up to the next exact
    // second. This test checks that the nsec field still lies within the
    // range [0, 999999999].
    Time t1 = Now();
    Time t2 = t1.Add(Second - Duration(t1.Nanosecond()));
    int sec = (t1.Second() + 1) % 60;
    EXPECT_TRUE(t2.Second() == sec && t2.Nanosecond() == 0)
        << "sec = " << t2.Second() << ", nsec = " << t2.Nanosecond() << ", want sec = " << sec << ", nsec = 0";
}

namespace {

bool equalTimeAndZone(const Time& a, const Time& b) {
    auto [aname, aoffset] = a.Zone();
    auto [bname, boffset] = b.Zone();
    return a.Equal(b) && aoffset == boffset && aname.view() == bname.view();
}

std::string bytes_str(const bytes& b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

}  // namespace

TEST_F(GoTime, TimeJSON) {
    struct {
        Time time;
        std::string json;
    } jsonTests[] = {
        {Date(9999, Month(4), 12, 23, 20, 50, 520 * 1000000, UTC), R"("9999-04-12T23:20:50.52Z")"},
        {Date(1996, Month(12), 19, 16, 39, 57, 0, Local), R"("1996-12-19T16:39:57-08:00")"},
        {Date(0, Month(1), 1, 0, 0, 0, 1, FixedZone("", 1 * 60)), R"("0000-01-01T00:00:00.000000001+00:01")"},
        {Date(2020, Month(1), 1, 0, 0, 0, 0, FixedZone("", 23 * 60 * 60 + 59 * 60)), R"("2020-01-01T00:00:00+23:59")"},
    };
    for (const auto& tt : jsonTests) {
        auto [jsonBytes, err] = json::Marshal(tt.time);
        if (err) {
            ADD_FAILURE() << "json.Marshal error = " << S(err);
            continue;
        }
        if (bytes_str(jsonBytes) != tt.json) {
            ADD_FAILURE() << "JSON = " << bytes_str(jsonBytes) << ", want " << tt.json;
            continue;
        }
        Time jsonTime;
        if (auto uerr = json::Unmarshal(jsonBytes, &jsonTime)) {
            ADD_FAILURE() << "json.Unmarshal error = " << S(uerr);
            continue;
        }
        EXPECT_TRUE(equalTimeAndZone(jsonTime, tt.time)) << "Unmarshaled time = " << S(jsonTime.String())
                                                         << ", want " << S(tt.time.String());
    }
}

TEST_F(GoTime, UnmarshalInvalidTimes) {
    struct {
        std::string in;
        std::string want;
    } tests[] = {
        {"{}", "Time.UnmarshalJSON: input is not a JSON string"},
        {"[]", "Time.UnmarshalJSON: input is not a JSON string"},
        {R"("2000-01-01T1:12:34Z")", "<nil>"},
        {R"("2000-01-01T00:00:00,000Z")", "<nil>"},
        {R"("2000-01-01T00:00:00+24:00")", "<nil>"},
        {R"("2000-01-01T00:00:00+00:60")", "<nil>"},
        {R"("2000-01-01T00:00:00+123:45")",
         R"(parsing time "2000-01-01T00:00:00+123:45" as "2006-01-02T15:04:05Z07:00": cannot parse "+123:45" as "Z07:00")"},
    };
    for (const auto& tt : tests) {
        Time ts;
        error err = json::Unmarshal(tt.in, &ts);
        EXPECT_EQ(S(err), tt.want) << "Time.UnmarshalJSON(" << tt.in << ")";
        if (tt.in.starts_with('"') && tt.in.ends_with('"')) {
            std::string inner = tt.in.substr(1, tt.in.size() - 2);
            err = ts.UnmarshalText(as_bytes(inner));
            EXPECT_EQ(S(err), tt.want) << "Time.UnmarshalText(" << tt.in << ")";
        }
    }
}

TEST_F(GoTime, MarshalInvalidTimes) {
    struct {
        Time time;
        std::string want;
    } tests[] = {
        {Date(10000, Month(1), 1, 0, 0, 0, 0, UTC), "Time.MarshalJSON: year outside of range [0,9999]"},
        {Date(-998, Month(1), 1, 0, 0, 0, 0, UTC).Add(-Second), "Time.MarshalJSON: year outside of range [0,9999]"},
        {Date(0, Month(1), 1, 0, 0, 0, 0, UTC).Add(-Nanosecond), "Time.MarshalJSON: year outside of range [0,9999]"},
        {Date(2020, Month(1), 1, 0, 0, 0, 0, FixedZone("", 24 * 60 * 60)),
         "Time.MarshalJSON: timezone hour outside of range [0,23]"},
        {Date(2020, Month(1), 1, 0, 0, 0, 0, FixedZone("", 123 * 60 * 60)),
         "Time.MarshalJSON: timezone hour outside of range [0,23]"},
    };
    auto replace = [](std::string s, std::string_view from, std::string_view to) {
        for (std::size_t i = s.find(from); i != s.npos; i = s.find(from, i + to.size())) s.replace(i, from.size(), to);
        return s;
    };
    for (const auto& tt : tests) {
        auto [b, err] = tt.time.MarshalJSON();
        EXPECT_TRUE(b.is_nil()) << "MarshalJSON() = " << bytes_str(b) << ", want nil";
        EXPECT_EQ(S(err), tt.want);

        auto [b2, err2] = tt.time.MarshalText();
        EXPECT_TRUE(b2.is_nil());
        EXPECT_EQ(S(err2), replace(tt.want, "JSON", "Text"));

        auto [b3, err3] = tt.time.AppendText(bytes{});
        EXPECT_TRUE(b3.is_nil());
        EXPECT_EQ(S(err3), replace(tt.want, "MarshalJSON", "AppendText"));
    }
}

TEST_F(GoTime, ParseDuration) {
    struct {
        std::string in;
        Duration want;
    } parseDurationTests[] = {
        {"0", Duration(0)},
        {"5s", 5 * Second},
        {"30s", 30 * Second},
        {"1478s", 1478 * Second},
        // sign
        {"-5s", -5 * Second},
        {"+5s", 5 * Second},
        {"-0", Duration(0)},
        {"+0", Duration(0)},
        // decimal
        {"5.0s", 5 * Second},
        {"5.6s", 5 * Second + 600 * Millisecond},
        {"5.s", 5 * Second},
        {".5s", 500 * Millisecond},
        {"1.0s", 1 * Second},
        {"1.00s", 1 * Second},
        {"1.004s", 1 * Second + 4 * Millisecond},
        {"1.0040s", 1 * Second + 4 * Millisecond},
        {"100.00100s", 100 * Second + 1 * Millisecond},
        // different units
        {"10ns", 10 * Nanosecond},
        {"11us", 11 * Microsecond},
        {"12µs", 12 * Microsecond},  // U+00B5
        {"12μs", 12 * Microsecond},  // U+03BC
        {"13ms", 13 * Millisecond},
        {"14s", 14 * Second},
        {"15m", 15 * Minute},
        {"16h", 16 * Hour},
        // composite durations
        {"3h30m", 3 * Hour + 30 * Minute},
        {"10.5s4m", 4 * Minute + 10 * Second + 500 * Millisecond},
        {"-2m3.4s", -(2 * Minute + 3 * Second + 400 * Millisecond)},
        {"1h2m3s4ms5us6ns", 1 * Hour + 2 * Minute + 3 * Second + 4 * Millisecond + 5 * Microsecond + 6 * Nanosecond},
        {"39h9m14.425s", 39 * Hour + 9 * Minute + 14 * Second + 425 * Millisecond},
        // large value
        {"52763797000ns", 52763797000 * Nanosecond},
        // more than 9 digits after decimal point, see https://golang.org/issue/6617
        {"0.3333333333333333333h", 20 * Minute},
        // 9007199254740993 = 1<<53+1 cannot be stored precisely in a float64
        {"9007199254740993ns", ((int64(1) << 53) + 1) * Nanosecond},
        // largest duration that can be represented by int64 in nanoseconds
        {"9223372036854775807ns", maxDuration},
        {"9223372036854775.807us", maxDuration},
        {"9223372036s854ms775us807ns", maxDuration},
        {"-9223372036854775808ns", minDuration},
        {"-9223372036854775.808us", minDuration},
        {"-9223372036s854ms775us808ns", minDuration},
        // largest negative value
        {"-9223372036854775808ns", minDuration},
        // largest negative round trip value, see https://golang.org/issue/48629
        {"-2562047h47m16.854775808s", minDuration},
        // huge string; issue 15011.
        {"0.100000000000000000000h", 6 * Minute},
        // This value tests the first overflow check in leadingFraction.
        {"0.830103483285477580700h", 49 * Minute + 48 * Second + 372539827 * Nanosecond},
    };
    for (const auto& tc : parseDurationTests) {
        auto [d, err] = ParseDuration(tc.in);
        EXPECT_TRUE(!err && d == tc.want) << "ParseDuration(" << tc.in << ") = " << d.ns << ", " << S(err) << ", want "
                                          << tc.want.ns;
    }
}

TEST_F(GoTime, ParseDurationErrors) {
    struct {
        std::string in;
        std::string expect;
    } parseDurationErrorTests[] = {
        // invalid
        {"", R"("")"},
        {"3", R"("3")"},
        {"-", R"("-")"},
        {"s", R"("s")"},
        {".", R"(".")"},
        {"-.", R"("-.")"},
        {".s", R"(".s")"},
        {"+.s", R"("+.s")"},
        {"1d", R"("1d")"},
        {"\x85\x85", R"("\x85\x85")"},
        {"\xff" "ff", R"("\xffff")"},
        {"hello \xff" "ff world", R"("hello \xffff world")"},
        {"�", R"("\xef\xbf\xbd")"},                                          // utf8.RuneError
        {"� hello � world", R"("\xef\xbf\xbd hello \xef\xbf\xbd world")"},  // utf8.RuneError
        // overflow
        {"9223372036854775810ns", R"("9223372036854775810ns")"},
        {"9223372036854775808ns", R"("9223372036854775808ns")"},
        {"-9223372036854775809ns", R"("-9223372036854775809ns")"},
        {"9223372036854776us", R"("9223372036854776us")"},
        {"3000000h", R"("3000000h")"},
        {"9223372036854775.808us", R"("9223372036854775.808us")"},
        {"9223372036854ms775us808ns", R"("9223372036854ms775us808ns")"},
    };
    for (const auto& tc : parseDurationErrorTests) {
        auto [_, err] = ParseDuration(tc.in);
        ASSERT_TRUE(err) << "ParseDuration(" << tc.in << ") = _, nil, want _, non-nil";
        EXPECT_NE(S(err).find(tc.expect), std::string::npos)
            << "ParseDuration(" << tc.in << ") = _, " << S(err) << ", error does not contain " << tc.expect;
    }
}

TEST_F(GoTime, ParseDurationRoundTrip) {
    auto [max1, err] = ParseDuration(maxDuration.String());
    EXPECT_TRUE(!err && max1 == maxDuration) << S(maxDuration.String()) << " " << S(err);
    auto [min1, err2] = ParseDuration(minDuration.String());
    EXPECT_TRUE(!err2 && min1 == minDuration) << S(minDuration.String()) << " " << S(err2);
    std::mt19937 rng(7);
    for (int i = 0; i < 100; i++) {
        Duration d0 = static_cast<int64>(rng() & 0x7fffffff) * Millisecond;
        string s = d0.String();
        auto [d1, err3] = ParseDuration(s);
        EXPECT_TRUE(!err3 && d0 == d1) << "round-trip failed: " << d0.ns << " => " << S(s) << " => " << d1.ns;
    }
}

TEST_F(GoTime, LocationRace) {
    hooks::ResetLocalOnceForTest();  // reset the Once to trigger the race
    auto c = make_chan<string>(1);
    go([c] { c.send(Now().String()); });
    (void)Now().String();
    c.recv();
    Sleep(100 * Millisecond);
    hooks::ForceUSPacificForTesting();
}

TEST_F(GoTime, LoadFixed) {
    // Issue 4064: handle locations without any zone transitions.
    auto [loc, err] = LoadLocation("Etc/GMT+1");
    ASSERT_FALSE(err) << S(err);
    // The tzdata name for Etc/GMT+1 is "-01"; older tzdata said "GMT+1".
    auto [name, offset] = Now().In(loc).Zone();
    EXPECT_TRUE((name.view() == "GMT+1" || name.view() == "-01") && offset == -1 * 60 * 60)
        << "Now().In(loc).Zone() = " << name.view() << ", " << offset;
}

TEST_F(GoTime, Sub) {
    struct {
        Time t;
        Time u;
        Duration d;
    } subTests[] = {
        {Time{}, Time{}, Duration(0)},
        {Date(2009, Month(11), 23, 0, 0, 0, 1, UTC), Date(2009, Month(11), 23, 0, 0, 0, 0, UTC), Duration(1)},
        {Date(2009, Month(11), 23, 0, 0, 0, 0, UTC), Date(2009, Month(11), 24, 0, 0, 0, 0, UTC), -24 * Hour},
        {Date(2009, Month(11), 24, 0, 0, 0, 0, UTC), Date(2009, Month(11), 23, 0, 0, 0, 0, UTC), 24 * Hour},
        {Date(-2009, Month(11), 24, 0, 0, 0, 0, UTC), Date(-2009, Month(11), 23, 0, 0, 0, 0, UTC), 24 * Hour},
        {Time{}, Date(2109, Month(11), 23, 0, 0, 0, 0, UTC), minDuration},
        {Date(2109, Month(11), 23, 0, 0, 0, 0, UTC), Time{}, maxDuration},
        {Time{}, Date(-2109, Month(11), 23, 0, 0, 0, 0, UTC), maxDuration},
        {Date(-2109, Month(11), 23, 0, 0, 0, 0, UTC), Time{}, minDuration},
        {Date(2290, Month(1), 1, 0, 0, 0, 0, UTC), Date(2000, Month(1), 1, 0, 0, 0, 0, UTC),
         290 * 365 * 24 * Hour + 71 * 24 * Hour},
        {Date(2300, Month(1), 1, 0, 0, 0, 0, UTC), Date(2000, Month(1), 1, 0, 0, 0, 0, UTC), maxDuration},
        {Date(2000, Month(1), 1, 0, 0, 0, 0, UTC), Date(2290, Month(1), 1, 0, 0, 0, 0, UTC),
         -290 * 365 * 24 * Hour - 71 * 24 * Hour},
        {Date(2000, Month(1), 1, 0, 0, 0, 0, UTC), Date(2300, Month(1), 1, 0, 0, 0, 0, UTC), minDuration},
        {Date(2311, Month(11), 26, 02, 16, 47, 63535996, UTC), Date(2019, Month(8), 16, 2, 29, 30, 268436582, UTC),
         Duration(9223372036795099414)},
        {hooks::MinMonoTime(), hooks::MaxMonoTime(), minDuration},
        {hooks::MaxMonoTime(), hooks::MinMonoTime(), maxDuration},
    };
    int i = 0;
    for (const auto& st : subTests) {
        Duration got = st.t.Sub(st.u);
        EXPECT_EQ(got.ns, st.d.ns) << "#" << i << ": Sub(" << S(st.t.String()) << ", " << S(st.u.String()) << ")";
        i++;
    }
}

TEST_F(GoTime, DurationNanoseconds) {
    EXPECT_EQ(Duration(-1000).Nanoseconds(), -1000);
    EXPECT_EQ(Duration(-1).Nanoseconds(), -1);
    EXPECT_EQ(Duration(1).Nanoseconds(), 1);
    EXPECT_EQ(Duration(1000).Nanoseconds(), 1000);
}

TEST_F(GoTime, DurationMicroseconds) {
    EXPECT_EQ(Duration(-1000).Microseconds(), -1);
    EXPECT_EQ(Duration(1000).Microseconds(), 1);
}

TEST_F(GoTime, DurationMilliseconds) {
    EXPECT_EQ(Duration(-1000000).Milliseconds(), -1);
    EXPECT_EQ(Duration(1000000).Milliseconds(), 1);
}

TEST_F(GoTime, DurationSeconds) { EXPECT_EQ(Duration(300000000).Seconds(), 0.3); }

TEST_F(GoTime, DurationMinutes) {
    EXPECT_EQ(Duration(-60000000000).Minutes(), -1);
    EXPECT_EQ(Duration(-1).Minutes(), -1 / 60e9);
    EXPECT_EQ(Duration(1).Minutes(), 1 / 60e9);
    EXPECT_EQ(Duration(60000000000).Minutes(), 1);
    EXPECT_EQ(Duration(3000).Minutes(), 5e-8);
}

TEST_F(GoTime, DurationHours) {
    EXPECT_EQ(Duration(-3600000000000).Hours(), -1);
    EXPECT_EQ(Duration(-1).Hours(), -1 / 3600e9);
    EXPECT_EQ(Duration(1).Hours(), 1 / 3600e9);
    EXPECT_EQ(Duration(3600000000000).Hours(), 1);
    EXPECT_EQ(Duration(36).Hours(), 1e-11);
}

TEST_F(GoTime, DurationTruncate) {
    struct {
        Duration d, m, want;
    } tests[] = {
        {Duration(0), Second, Duration(0)},
        {Minute, -7 * Second, Minute},
        {Minute, Duration(0), Minute},
        {Minute, Duration(1), Minute},
        {Minute + 10 * Second, 10 * Second, Minute + 10 * Second},
        {2 * Minute + 10 * Second, Minute, 2 * Minute},
        {10 * Minute + 10 * Second, 3 * Minute, 9 * Minute},
        {Minute + 10 * Second, Minute + 10 * Second + Duration(1), Duration(0)},
        {Minute + 10 * Second, Hour, Duration(0)},
        {-Minute, Second, -Minute},
        {-10 * Minute, 3 * Minute, -9 * Minute},
        {-10 * Minute, Hour, Duration(0)},
    };
    for (const auto& tt : tests) {
        EXPECT_EQ(tt.d.Truncate(tt.m).ns, tt.want.ns) << S(tt.d.String()) << ".Truncate(" << S(tt.m.String()) << ")";
    }
}

TEST_F(GoTime, DurationRound) {
    struct {
        Duration d, m, want;
    } tests[] = {
        {Duration(0), Second, Duration(0)},
        {Minute, -11 * Second, Minute},
        {Minute, Duration(0), Minute},
        {Minute, Duration(1), Minute},
        {2 * Minute, Minute, 2 * Minute},
        {2 * Minute + 10 * Second, Minute, 2 * Minute},
        {2 * Minute + 30 * Second, Minute, 3 * Minute},
        {2 * Minute + 50 * Second, Minute, 3 * Minute},
        {-Minute, Duration(1), -Minute},
        {-2 * Minute, Minute, -2 * Minute},
        {-2 * Minute - 10 * Second, Minute, -2 * Minute},
        {-2 * Minute - 30 * Second, Minute, -3 * Minute},
        {-2 * Minute - 50 * Second, Minute, -3 * Minute},
        {Duration(8000000000000000000), Duration(3000000000000000000), Duration(9000000000000000000)},
        {Duration(9000000000000000000), Duration(5000000000000000000), maxDuration},
        {Duration(-8000000000000000000), Duration(3000000000000000000), Duration(-9000000000000000000)},
        {Duration(-9000000000000000000), Duration(5000000000000000000), minDuration},
        {Duration((int64(3) << 61) - 1), Duration(int64(3) << 61), Duration(int64(3) << 61)},
    };
    for (const auto& tt : tests) {
        EXPECT_EQ(tt.d.Round(tt.m).ns, tt.want.ns) << S(tt.d.String()) << ".Round(" << S(tt.m.String()) << ")";
    }
}

TEST_F(GoTime, DurationAbs) {
    struct {
        Duration d, want;
    } tests[] = {
        {Duration(0), Duration(0)},
        {Duration(1), Duration(1)},
        {Duration(-1), Duration(1)},
        {1 * Minute, 1 * Minute},
        {-1 * Minute, 1 * Minute},
        {minDuration, maxDuration},
        {minDuration + Duration(1), maxDuration},
        {minDuration + Duration(2), maxDuration - Duration(1)},
        {maxDuration, maxDuration},
        {maxDuration - Duration(1), maxDuration - Duration(1)},
    };
    for (const auto& tt : tests) EXPECT_EQ(tt.d.Abs().ns, tt.want.ns) << S(tt.d.String());
}

TEST_F(GoTime, DefaultLoc) {
    // Verify that all of Time's methods behave identically if loc is set
    // to nil or UTC.
    auto eqb = [](std::pair<bytes, error> a, std::pair<bytes, error> b) {
        return bytes_str(a.first) == bytes_str(b.first) && S(a.second) == S(b.second);
    };
    struct {
        std::string name;
        std::function<bool(const Time&, const Time&)> f;
    } tests[] = {
        {"After", [](const Time& t1, const Time& t2) { return t1.After(t2) == t2.After(t1); }},
        {"Before", [](const Time& t1, const Time& t2) { return t1.Before(t2) == t2.Before(t1); }},
        {"Equal", [](const Time& t1, const Time& t2) { return t1.Equal(t2) == t2.Equal(t1); }},
        {"Compare", [](const Time& t1, const Time& t2) { return t1.Compare(t2) == t2.Compare(t1); }},
        {"IsZero", [](const Time& t1, const Time& t2) { return t1.IsZero() == t2.IsZero(); }},
        {"Date", [](const Time& t1, const Time& t2) { return t1.Date() == t2.Date(); }},
        {"Year", [](const Time& t1, const Time& t2) { return t1.Year() == t2.Year(); }},
        {"Month", [](const Time& t1, const Time& t2) { return t1.Month() == t2.Month(); }},
        {"Day", [](const Time& t1, const Time& t2) { return t1.Day() == t2.Day(); }},
        {"Weekday", [](const Time& t1, const Time& t2) { return t1.Weekday() == t2.Weekday(); }},
        {"ISOWeek", [](const Time& t1, const Time& t2) { return t1.ISOWeek() == t2.ISOWeek(); }},
        {"Clock", [](const Time& t1, const Time& t2) { return t1.Clock() == t2.Clock(); }},
        {"Hour", [](const Time& t1, const Time& t2) { return t1.Hour() == t2.Hour(); }},
        {"Minute", [](const Time& t1, const Time& t2) { return t1.Minute() == t2.Minute(); }},
        {"Second", [](const Time& t1, const Time& t2) { return t1.Second() == t2.Second(); }},
        {"Nanosecond", [](const Time& t1, const Time& t2) { return t1.Hour() == t2.Hour(); }},
        {"YearDay", [](const Time& t1, const Time& t2) { return t1.YearDay() == t2.YearDay(); }},
        {"Add", [](const Time& t1, const Time& t2) { return t1.Add(Hour).Equal(t2.Add(Hour)); }},
        {"Sub", [](const Time& t1, const Time& t2) { return t1.Sub(t2) == t2.Sub(t1); }},
        {"AddDate", [](const Time& t1, const Time& t2) { return identical(t1.AddDate(1991, 9, 3), t2.AddDate(1991, 9, 3)); }},
        {"UTC", [](const Time& t1, const Time& t2) { return identical(t1.UTC(), t2.UTC()); }},
        {"Local", [](const Time& t1, const Time& t2) { return identical(t1.Local(), t2.Local()); }},
        {"In", [](const Time& t1, const Time& t2) { return identical(t1.In(UTC), t2.In(UTC)); }},
        {"Zone", [](const Time& t1, const Time& t2) {
             auto [a1, b1] = t1.Zone();
             auto [a2, b2] = t2.Zone();
             return a1.view() == a2.view() && b1 == b2;
         }},
        {"Unix", [](const Time& t1, const Time& t2) { return t1.Unix() == t2.Unix(); }},
        {"UnixNano", [](const Time& t1, const Time& t2) { return t1.UnixNano() == t2.UnixNano(); }},
        {"UnixMilli", [](const Time& t1, const Time& t2) { return t1.UnixMilli() == t2.UnixMilli(); }},
        {"UnixMicro", [](const Time& t1, const Time& t2) { return t1.UnixMicro() == t2.UnixMicro(); }},
        {"MarshalJSON", [&](const Time& t1, const Time& t2) { return eqb(t1.MarshalJSON(), t2.MarshalJSON()); }},
        {"AppendText", [&](const Time& t1, const Time& t2) { return eqb(t1.AppendText({}), t2.AppendText({})); }},
        {"MarshalText", [&](const Time& t1, const Time& t2) { return eqb(t1.MarshalText(), t2.MarshalText()); }},
        {"Truncate", [](const Time& t1, const Time& t2) { return t1.Truncate(Hour).Equal(t2.Truncate(Hour)); }},
        {"Round", [](const Time& t1, const Time& t2) { return t1.Round(Hour).Equal(t2.Round(Hour)); }},
        {"== Time{}", [](const Time& t1, const Time& t2) { return identical(t1, Time{}) == identical(t2, Time{}); }},
    };
    for (const auto& tt : tests) {
        Time t1;
        Time t2 = Time{}.UTC();
        EXPECT_TRUE(tt.f(t1, t2)) << "Time{} and Time{}.UTC() behave differently for " << tt.name;
    }
}

TEST_F(GoTime, ZeroMonthString) { EXPECT_EQ(S(Month(0).String()), "%!Month(0)"); }

TEST_F(GoTime, WeekdayString) {
    EXPECT_EQ(S(Tuesday.String()), "Tuesday");
    EXPECT_EQ(S(Weekday(14).String()), "%!Weekday(14)");
}

TEST_F(GoTime, ReadFileLimit) {
    auto [_, err] = hooks::ReadFile("/dev/zero");
    EXPECT_NE(S(err).find("is too large"), std::string::npos) << S(err);
}

TEST_F(GoTime, TimeIsDST) {
    auto [tzWithDST, err] = LoadLocation("Australia/Sydney");
    ASSERT_FALSE(err) << "could not load tz 'Australia/Sydney': " << S(err);
    auto [tzWithoutDST, err2] = LoadLocation("Australia/Brisbane");
    ASSERT_FALSE(err2) << "could not load tz 'Australia/Brisbane': " << S(err2);
    const Location* tzFixed = FixedZone("FIXED_TIME", 12345);
    struct {
        Time time;
        bool want;
    } tests[] = {
        {Date(2009, Month(1), 1, 12, 0, 0, 0, UTC), false},
        {Date(2009, Month(6), 1, 12, 0, 0, 0, UTC), false},
        {Date(2009, Month(1), 1, 12, 0, 0, 0, tzWithDST), true},
        {Date(2009, Month(6), 1, 12, 0, 0, 0, tzWithDST), false},
        {Date(2009, Month(1), 1, 12, 0, 0, 0, tzWithoutDST), false},
        {Date(2009, Month(6), 1, 12, 0, 0, 0, tzWithoutDST), false},
        {Date(2009, Month(1), 1, 12, 0, 0, 0, tzFixed), false},
        {Date(2009, Month(6), 1, 12, 0, 0, 0, tzFixed), false},
    };
    int i = 0;
    for (const auto& tt : tests) {
        EXPECT_EQ(tt.time.IsDST(), tt.want) << "#" << i << ": " << S(tt.time.Format(RFC3339));
        i++;
    }
}

TEST_F(GoTime, TimeAddSecOverflow) {
    // Test it with positive delta.
    int64 maxInt64 = INT64_MAX;
    int64 timeExt = maxInt64 - unixToInternal - 50;
    Time notMonoTime = Unix(timeExt, 0);
    for (int64 i = 0; i < 100; i++) {
        int64 sec = notMonoTime.Unix();
        notMonoTime = notMonoTime.Add(Duration(i * 1000000000));
        int64 newSec = notMonoTime.Unix();
        ASSERT_TRUE(newSec == sec + i || newSec + unixToInternal == maxInt64)
            << "time ext: " << newSec << " overflows with positive delta, overflow threshold: " << maxInt64;
    }

    // Test it with negative delta.
    maxInt64 = -maxInt64;
    notMonoTime = hooks::NotMonoNegativeTime();
    for (int64 i = 0; i > -100; i--) {
        int64 sec = notMonoTime.Unix();
        notMonoTime = notMonoTime.Add(Duration(i * 1000000000));
        int64 newSec = notMonoTime.Unix();
        ASSERT_TRUE(newSec == sec + i || newSec + unixToInternal == maxInt64)
            << "time ext: " << newSec << " overflows with negative delta, overflow threshold: " << maxInt64;
    }
}

// Issue 49284: time: ParseInLocation incorrectly because of Daylight
// Saving Time
TEST_F(GoTime, TimeWithZoneTransition) {
    auto [loc, err] = LoadLocation("Asia/Shanghai");
    ASSERT_FALSE(err) << S(err);
    struct {
        Time give;
        Time want;
    } tests[] = {
        // 14 Apr 1991 - Daylight Saving Time Started
        // When time of "Asia/Shanghai" was about to reach
        // Sunday, 14 April 1991, 02:00:00 clocks were turned forward 1 hour to
        // Sunday, 14 April 1991, 03:00:00 local daylight time instead.
        // The UTC time was 13 April 1991, 18:00:00
        {Date(1991, April, 13, 17, 50, 0, 0, loc), Date(1991, April, 13, 9, 50, 0, 0, UTC)},
        {Date(1991, April, 13, 18, 0, 0, 0, loc), Date(1991, April, 13, 10, 0, 0, 0, UTC)},
        {Date(1991, April, 14, 1, 50, 0, 0, loc), Date(1991, April, 13, 17, 50, 0, 0, UTC)},
        {Date(1991, April, 14, 3, 0, 0, 0, loc), Date(1991, April, 13, 18, 0, 0, 0, UTC)},

        // 15 Sep 1991 - Daylight Saving Time Ended
        // When local daylight time of "Asia/Shanghai" was about to reach
        // Sunday, 15 September 1991, 02:00:00 clocks were turned backward 1 hour to
        // Sunday, 15 September 1991, 01:00:00 local standard time instead.
        // The UTC time was 14 September 1991, 17:00:00
        {Date(1991, September, 14, 16, 50, 0, 0, loc), Date(1991, September, 14, 7, 50, 0, 0, UTC)},
        {Date(1991, September, 14, 17, 0, 0, 0, loc), Date(1991, September, 14, 8, 0, 0, 0, UTC)},
        {Date(1991, September, 15, 0, 50, 0, 0, loc), Date(1991, September, 14, 15, 50, 0, 0, UTC)},
        {Date(1991, September, 15, 2, 00, 0, 0, loc), Date(1991, September, 14, 18, 00, 0, 0, UTC)},
    };
    int i = 0;
    for (const auto& tt : tests) {
        EXPECT_TRUE(tt.give.Equal(tt.want))
            << "#" << i << ": " << S(tt.give.Format(RFC3339)) << " is not equal to " << S(tt.want.Format(RFC3339));
        i++;
    }
}

TEST_F(GoTime, ZoneBounds) {
    auto [loc, err] = LoadLocation("Asia/Shanghai");
    ASSERT_FALSE(err) << S(err);

    // The ZoneBounds of a UTC location would just return two zero Time.
    for (const auto& test : utctests) {
        Time tm = Unix(test.seconds, 0).UTC();
        auto [start, end] = tm.ZoneBounds();
        EXPECT_TRUE(start.IsZero() && end.IsZero()) << "ZoneBounds of " << test.seconds << " expects two zero Time";
    }

    // If the zone begins at the beginning of time, start will be returned
    // as a zero Time.
    Time beginTime = Date(INT32_MIN, January, 1, 0, 0, 0, 0, loc);
    auto [start1, end1] = beginTime.ZoneBounds();
    EXPECT_TRUE(start1.IsZero() && !end1.IsZero()) << "ZoneBounds of " << S(beginTime.String()) << " start "
                                                   << S(start1.String()) << " end " << S(end1.String());

    // If the zone goes on forever, end will be returned as a zero Time.
    Time foreverTime = Date(INT32_MAX, January, 1, 0, 0, 0, 0, loc);
    auto [start2, end2] = foreverTime.ZoneBounds();
    EXPECT_TRUE(!start2.IsZero() && end2.IsZero()) << "ZoneBounds of " << S(foreverTime.String()) << " start "
                                                   << S(start2.String()) << " end " << S(end2.String());

    // Check some real-world cases to make sure we're getting the right
    // bounds.
    Time boundOne = Date(1990, September, 16, 1, 0, 0, 0, loc);
    Time boundTwo = Date(1991, April, 14, 3, 0, 0, 0, loc);
    Time boundThree = Date(1991, September, 15, 1, 0, 0, 0, loc);
    auto makeLocalTime = [](int64 sec) { return Unix(sec, 0); };
    struct {
        Time giveTime;
        Time wantStart;
        Time wantEnd;
    } realTests[] = {
        // The ZoneBounds of "Asia/Shanghai" Daylight Saving Time
        {Date(1991, April, 13, 17, 50, 0, 0, loc), boundOne, boundTwo},
        {Date(1991, April, 13, 18, 0, 0, 0, loc), boundOne, boundTwo},
        {Date(1991, April, 14, 1, 50, 0, 0, loc), boundOne, boundTwo},
        {boundTwo, boundTwo, boundThree},
        {Date(1991, September, 14, 16, 50, 0, 0, loc), boundTwo, boundThree},
        {Date(1991, September, 14, 17, 0, 0, 0, loc), boundTwo, boundThree},
        {Date(1991, September, 15, 0, 50, 0, 0, loc), boundTwo, boundThree},

        // The ZoneBounds of a "Asia/Shanghai" after the last transition
        // (Standard Time)
        {boundThree, boundThree, Time{}},
        {Date(1991, December, 15, 1, 50, 0, 0, loc), boundThree, Time{}},
        {Date(1992, April, 13, 17, 50, 0, 0, loc), boundThree, Time{}},
        {Date(1992, April, 13, 18, 0, 0, 0, loc), boundThree, Time{}},
        {Date(1992, April, 14, 1, 50, 0, 0, loc), boundThree, Time{}},
        {Date(1992, September, 14, 16, 50, 0, 0, loc), boundThree, Time{}},
        {Date(1992, September, 14, 17, 0, 0, 0, loc), boundThree, Time{}},
        {Date(1992, September, 15, 0, 50, 0, 0, loc), boundThree, Time{}},

        // The ZoneBounds of a local time would return two local Time. Note:
        // We preloaded "America/Los_Angeles" as time.Local for testing
        {makeLocalTime(0), makeLocalTime(-5756400), makeLocalTime(9972000)},
        {makeLocalTime(1221681866), makeLocalTime(1205056800), makeLocalTime(1225616400)},
        {makeLocalTime(2152173599), makeLocalTime(2145916800), makeLocalTime(2152173600)},
        {makeLocalTime(2152173600), makeLocalTime(2152173600), makeLocalTime(2172733200)},
        {makeLocalTime(2152173601), makeLocalTime(2152173600), makeLocalTime(2172733200)},
        {makeLocalTime(2159200800), makeLocalTime(2152173600), makeLocalTime(2172733200)},
        {makeLocalTime(2172733199), makeLocalTime(2152173600), makeLocalTime(2172733200)},
        {makeLocalTime(2172733200), makeLocalTime(2172733200), makeLocalTime(2177452800)},
    };
    int i = 0;
    for (const auto& tt : realTests) {
        auto [start, end] = tt.giveTime.ZoneBounds();
        EXPECT_TRUE(start.Equal(tt.wantStart) && end.Equal(tt.wantEnd))
            << "#" << i << ": ZoneBounds of " << S(tt.giveTime.String()) << ": got start=" << S(start.String())
            << " want start=" << S(tt.wantStart.String()) << " got end=" << S(end.String())
            << " want end=" << S(tt.wantEnd.String());
        i++;
    }
}

// ---------------------------------------------------------------------------
// mono_test.go

TEST_F(GoTime, HasMonotonicClock) {
    auto yes = [](const char* expr, const Time& tt) {
        EXPECT_NE(Time::internal::mono(tt), 0) << expr << ": missing monotonic clock reading";
    };
    auto no = [](const char* expr, const Time& tt) {
        EXPECT_EQ(Time::internal::mono(tt), 0) << expr << ": unexpected monotonic clock reading";
    };

    yes("<-After(1)", After(Duration(1)).recv());
    auto ticker = NewTicker(Duration(1));
    yes("<-Tick(1)", ticker->C.recv());
    ticker->Stop();
    no("Date(2009, 11, 23, 0, 0, 0, 0, UTC)", Date(2009, Month(11), 23, 0, 0, 0, 0, UTC));
    auto [tp, _] = Parse(UnixDate, "Sat Mar  7 11:06:39 PST 2015");
    no(R"(Parse(UnixDate, "Sat Mar  7 11:06:39 PST 2015"))", tp);
    no("Unix(1486057371, 0)", Unix(1486057371, 0));

    yes("Now()", Now());

    Time tu = Unix(1486057371, 0);
    Time tm = tu;
    Time::internal::setMono(tm, 123456);
    no("tu", tu);
    yes("tm", tm);

    no("tu.Add(1)", tu.Add(Duration(1)));
    no("tu.In(UTC)", tu.In(UTC));
    no("tu.AddDate(1, 1, 1)", tu.AddDate(1, 1, 1));
    no("tu.AddDate(0, 0, 0)", tu.AddDate(0, 0, 0));
    no("tu.Local()", tu.Local());
    no("tu.UTC()", tu.UTC());
    no("tu.Round(2)", tu.Round(Duration(2)));
    no("tu.Truncate(2)", tu.Truncate(Duration(2)));

    yes("tm.Add(1)", tm.Add(Duration(1)));
    no("tm.AddDate(1, 1, 1)", tm.AddDate(1, 1, 1));
    no("tm.AddDate(0, 0, 0)", tm.AddDate(0, 0, 0));
    no("tm.In(UTC)", tm.In(UTC));
    no("tm.Local()", tm.Local());
    no("tm.UTC()", tm.UTC());
    no("tm.Round(2)", tm.Round(Duration(2)));
    no("tm.Truncate(2)", tm.Truncate(Duration(2)));
}

TEST_F(GoTime, MonotonicAdd) {
    Time tm = Unix(1486057371, 123456);
    Time::internal::setMono(tm, 123456789012345);

    Time t2 = tm.Add(Duration(100000000));
    EXPECT_EQ(t2.Nanosecond(), 100123456);
    EXPECT_EQ(Time::internal::mono(t2), 123456889012345);

    Time t3 = tm.Add(Duration(-9000000000000000000));  // wall now out of range
    EXPECT_EQ(t3.Nanosecond(), 123456);
    EXPECT_EQ(Time::internal::mono(t3), 0) << "wall time out of range for monotonic reading";

    Time t4 = tm.Add(Duration(+9000000000000000000));  // wall now out of range
    EXPECT_EQ(t4.Nanosecond(), 123456);
    EXPECT_EQ(Time::internal::mono(t4), 0) << "wall time out of range for monotonic reading";

    Time tn = Now();
    Time tn1 = tn.Add(1 * Hour);
    Sleep(100 * Millisecond);
    Duration d = Until(tn1);
    EXPECT_GE(d.ns, (59 * Minute).ns) << "Until(Now().Add(1*Hour)) = " << S(d.String());
    Time now = Now();
    EXPECT_FALSE(now.After(tn1));
    EXPECT_TRUE(tn1.After(now));
    EXPECT_FALSE(tn1.Before(now));
    EXPECT_TRUE(now.Before(tn1));
    EXPECT_EQ(now.Compare(tn1), -1);
    EXPECT_EQ(tn1.Compare(now), 1);
}

TEST_F(GoTime, MonotonicSub) {
    Time t1 = Unix(1483228799, 995000000);
    Time::internal::setMono(t1, 123456789012345);

    Time t2 = Unix(1483228799, 5000000);
    Time::internal::setMono(t2, 123456789012345 + 10000000);

    Time t3 = Unix(1483228799, 995000000);
    Time::internal::setMono(t3, 123456789012345 + 1000000000);

    Time t1w = t1.AddDate(0, 0, 0);
    ASSERT_EQ(Time::internal::mono(t1w), 0) << "AddDate didn't strip monotonic clock reading";
    Time t2w = t2.AddDate(0, 0, 0);
    ASSERT_EQ(Time::internal::mono(t2w), 0) << "AddDate didn't strip monotonic clock reading";
    Time t3w = t3.AddDate(0, 0, 0);
    ASSERT_EQ(Time::internal::mono(t3w), 0) << "AddDate didn't strip monotonic clock reading";

    auto sub = [](const std::string& txs, const std::string& tys, const Time& tx, const Time& txw, const Time& ty,
                  const Time& tyw, Duration d, Duration dw) {
        EXPECT_EQ(tx.Sub(ty).ns, d.ns) << txs << ".Sub(" << tys << ")";
        EXPECT_EQ(txw.Sub(ty).ns, dw.ns) << txs << "w.Sub(" << tys << ")";
        EXPECT_EQ(tx.Sub(tyw).ns, dw.ns) << txs << ".Sub(" << tys << "w)";
        EXPECT_EQ(txw.Sub(tyw).ns, dw.ns) << txs << "w.Sub(" << tys << "w)";
    };
    sub("t1", "t1", t1, t1w, t1, t1w, Duration(0), Duration(0));
    sub("t1", "t2", t1, t1w, t2, t2w, -10 * Millisecond, 990 * Millisecond);
    sub("t1", "t3", t1, t1w, t3, t3w, -1000 * Millisecond, Duration(0));

    sub("t2", "t1", t2, t2w, t1, t1w, 10 * Millisecond, -990 * Millisecond);
    sub("t2", "t2", t2, t2w, t2, t2w, Duration(0), Duration(0));
    sub("t2", "t3", t2, t2w, t3, t3w, -990 * Millisecond, -990 * Millisecond);

    sub("t3", "t1", t3, t3w, t1, t1w, 1000 * Millisecond, Duration(0));
    sub("t3", "t2", t3, t3w, t2, t2w, 990 * Millisecond, 990 * Millisecond);
    sub("t3", "t3", t3, t3w, t3, t3w, Duration(0), Duration(0));

    auto cmp = [](const std::string& txs, const std::string& tys, const Time& tx, const Time& txw, const Time& ty,
                  const Time& tyw, int c, int cw) {
        EXPECT_EQ(tx.After(ty), c > 0) << txs << ".After(" << tys << ")";
        EXPECT_EQ(txw.After(ty), cw > 0) << txs << "w.After(" << tys << ")";
        EXPECT_EQ(tx.After(tyw), cw > 0) << txs << ".After(" << tys << "w)";
        EXPECT_EQ(txw.After(tyw), cw > 0) << txs << "w.After(" << tys << "w)";

        EXPECT_EQ(tx.Before(ty), c < 0) << txs << ".Before(" << tys << ")";
        EXPECT_EQ(txw.Before(ty), cw < 0) << txs << "w.Before(" << tys << ")";
        EXPECT_EQ(tx.Before(tyw), cw < 0) << txs << ".Before(" << tys << "w)";
        EXPECT_EQ(txw.Before(tyw), cw < 0) << txs << "w.Before(" << tys << "w)";

        EXPECT_EQ(tx.Equal(ty), c == 0) << txs << ".Equal(" << tys << ")";
        EXPECT_EQ(txw.Equal(ty), cw == 0) << txs << "w.Equal(" << tys << ")";
        EXPECT_EQ(tx.Equal(tyw), cw == 0) << txs << ".Equal(" << tys << "w)";
        EXPECT_EQ(txw.Equal(tyw), cw == 0) << txs << "w.Equal(" << tys << "w)";

        EXPECT_EQ(tx.Compare(ty), c) << txs << ".Compare(" << tys << ")";
        EXPECT_EQ(txw.Compare(ty), cw) << txs << "w.Compare(" << tys << ")";
        EXPECT_EQ(tx.Compare(tyw), cw) << txs << ".Compare(" << tys << "w)";
        EXPECT_EQ(txw.Compare(tyw), cw) << txs << "w.Compare(" << tys << "w)";
    };

    cmp("t1", "t1", t1, t1w, t1, t1w, 0, 0);
    cmp("t1", "t2", t1, t1w, t2, t2w, -1, +1);
    cmp("t1", "t3", t1, t1w, t3, t3w, -1, 0);

    cmp("t2", "t1", t2, t2w, t1, t1w, +1, -1);
    cmp("t2", "t2", t2, t2w, t2, t2w, 0, 0);
    cmp("t2", "t3", t2, t2w, t3, t3w, -1, -1);

    cmp("t3", "t1", t3, t3w, t1, t1w, +1, 0);
    cmp("t3", "t2", t3, t3w, t2, t2w, +1, +1);
    cmp("t3", "t3", t3, t3w, t3, t3w, 0, 0);
}

TEST_F(GoTime, MonotonicOverflow) {
    Time t1 = Now().Add(-30 * Second);
    Duration d = Until(t1);
    EXPECT_TRUE(-35 * Second <= d && d <= -30 * Second) << "Until(Now().Add(-30s)) = " << S(d.String());

    t1 = Now().Add(30 * Second);
    d = Until(t1);
    EXPECT_TRUE(25 * Second <= d && d <= 30 * Second) << "Until(Now().Add(30s)) = " << S(d.String());

    Time t0 = Now();
    t1 = t0.Add(maxDuration);
    EXPECT_EQ(Time::internal::mono(t1), 0) << "Now().Add(maxDuration) has monotonic clock reading";
    Time t2 = t1.Add(-maxDuration);
    d = Since(t2);
    EXPECT_TRUE(-10 * Second <= d && d <= 10 * Second) << "Since(Now().Add(max).Add(-max)) = " << S(d.String());

    t0 = Now();
    t1 = t0.Add(1 * Hour);
    Sleep(100 * Millisecond);
    t2 = Now().Add(-5 * Second);
    EXPECT_TRUE(t1.After(t2));
    EXPECT_FALSE(t2.After(t1));
    EXPECT_FALSE(t1.Before(t2));
    EXPECT_TRUE(t2.Before(t1));
    EXPECT_EQ(t1.Compare(t2), 1);
    EXPECT_EQ(t2.Compare(t1), -1);
}

TEST_F(GoTime, MonotonicString) {
    struct {
        int64 mono;
        std::string want;
    } tests[] = {
        {0, "m=+0.000000000"},
        {123456789, "m=+0.123456789"},
        {-123456789, "m=-0.123456789"},
        {123456789000, "m=+123.456789000"},
        {-123456789000, "m=-123.456789000"},
        {9000000000000000000, "m=+9000000000.000000000"},
        {-9000000000000000000, "m=-9000000000.000000000"},
        {INT64_MIN, "m=-9223372036.854775808"},
    };
    for (const auto& tt : tests) {
        Time t1 = Now();
        Time::internal::setMono(t1, tt.mono);
        std::string s = S(t1.String());
        EXPECT_EQ(s.substr(s.rfind(' ') + 1), tt.want) << "with mono=" << tt.mono;
    }
}

// ---------------------------------------------------------------------------
// go++'s own

// Local and the zones load on first use, from many fibers at once: they
// must not deadlock on it (see Net.ConcurrentFirstUse). A fresh process
// with one P and no preemption, where a parked loader would hang.
namespace {

const bool time_first_use_helper = go_helper::register_helper("time-first-use", [](const std::vector<std::string>&) {
    sync::WaitGroup wg;
    for (int i = 0; i < 16; i++) {
        wg.Go([i] {
            if (i % 2 == 0) (void)Now().Format(RFC1123);
            else (void)LoadLocation(i % 4 == 1 ? "Asia/Tokyo" : "Europe/Paris");
        });
    }
    wg.Wait();
    os::Exit(0);
});

}  // namespace

TEST(GoppTime, ConcurrentFirstUse) {
    auto [ctx, cancel] = context::WithTimeout(context::Background(), 10 * Second);
    auto cmd = go_helper::helper_command_context(ctx, "time-first-use");
    cmd->Env = append(cmd->Environ(), string("GOMAXPROCS=1"), string("GODEBUG=asyncpreemptoff=1"));
    auto err = cmd->Run();
    EXPECT_FALSE(err) << S(err) << ": the fibers deadlocked on first use";
    cancel();
}
