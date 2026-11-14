// Ported from Go 1.27.3's time/formatTest.go, with Go's test names.
//
// As in Go, Local is America/Los_Angeles for these tests (from the
// system's zoneinfo; Go's tests use the zip file in its tree).
//
// Left out: TestLoadLocationZipFile (needs Go's zoneinfo.zip) and the
// fuzz tests. testing/quick in TestFormatAndParse is a fixed-seed random
// sample here.
#include "time/internal.hpp"

#include <gopp/errors.hpp>
#include <gopp/fmt.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::time;
namespace hooks = gopp::time::testing_hooks;

namespace {

class GoTimeFormat : public ::testing::Test {
protected:
    static void SetUpTestSuite() { hooks::ForceUSPacificForTesting(); }
    static void TearDownTestSuite() { hooks::ResetLocalOnceForTest(); }
};

std::string S(const string& s) { return std::string(s.view()); }

// Go's t1 == t2 on Times: the same instant, location and reading.
bool identical(const Time& a, const Time& b) {
    using I = Time::internal;
    return I::sec(a) == I::sec(b) && I::nsec(a) == I::nsec(b) && I::hasMono(a) == I::hasMono(b) &&
           I::mono(a) == I::mono(b) && I::loc(a) == I::loc(b);
}

}  // namespace

TEST_F(GoTimeFormat, NextStdChunk) {
    // markChunks returns format with each detected 'format chunk'
    // parenthesized. The chunk's text is what Go's StdChunkNames gives.
    auto markChunks = [](std::string format) {
        std::string out;
        for (std::string s = format; !s.empty();) {
            auto [prefix, std, suffix] = hooks::NextStdChunk(s);
            out += prefix;
            if (std > 0) out += "(" + s.substr(prefix.size(), s.size() - prefix.size() - suffix.size()) + ")";
            s = suffix;
        }
        return out;
    };
    const char* tests[] = {
        "(2006)-(01)-(02)T(15):(04):(05)(Z07:00)",
        "(2006)-(01)-(02) (002) (15):(04):(05)",
        "(2006)-(01) (002) (15):(04):(05)",
        "(2006)-(002) (15):(04):(05)",
        "(2006)(002)(01) (15):(04):(05)",
        "(2006)(002)(04) (15):(04):(05)",
    };
    for (std::string marked : tests) {
        std::string format;
        for (char c : marked) {
            if (c != '(' && c != ')') format += c;
        }
        EXPECT_EQ(markChunks(format), marked) << "nextStdChunk parses " << format;
    }
}

TEST_F(GoTimeFormat, RFC3339Conversion) {
    struct {
        Time time;
        std::string formattedValue;
    } tests[] = {
        {Date(2008, Month(9), 17, 20, 4, 26, 0, UTC), "2008-09-17T20:04:26Z"},
        {Date(1994, Month(9), 17, 20, 4, 26, 0, FixedZone("EST", -18000)), "1994-09-17T20:04:26-05:00"},
        {Date(2000, Month(12), 26, 1, 15, 6, 0, FixedZone("OTO", 15600)), "2000-12-26T01:15:06+04:20"},
    };
    for (const auto& f : tests) EXPECT_EQ(S(f.time.Format(RFC3339)), f.formattedValue);
}

TEST_F(GoTimeFormat, AppendInt) {
    struct {
        int in;
        int width;
        std::string want;
    } tests[] = {
        {0, 0, "0"},          {0, 1, "0"},          {0, 2, "00"},         {0, 3, "000"},
        {1, 0, "1"},          {1, 1, "1"},          {1, 2, "01"},         {1, 3, "001"},
        {-1, 0, "-1"},        {-1, 1, "-1"},        {-1, 2, "-01"},       {-1, 3, "-001"},
        {99, 2, "99"},        {100, 2, "100"},      {1, 4, "0001"},       {12, 4, "0012"},
        {123, 4, "0123"},     {1234, 4, "1234"},    {12345, 4, "12345"},  {1, 5, "00001"},
        {12, 5, "00012"},     {123, 5, "00123"},    {1234, 5, "01234"},   {12345, 5, "12345"},
        {123456, 5, "123456"}, {0, 9, "000000000"}, {123, 9, "000000123"}, {123456, 9, "000123456"},
        {123456789, 9, "123456789"},
    };
    for (const auto& tt : tests) {
        std::string got;
        appendInt(got, tt.in, tt.width);
        EXPECT_EQ(got, tt.want) << "appendInt(" << tt.in << ", " << tt.width << ")";
    }
}

namespace {

struct FormatTest {
    std::string name;
    std::string format;
    std::string result;
};

const std::vector<FormatTest> formatTests = {
    {"ANSIC", std::string(ANSIC), "Wed Feb  4 21:00:57 2009"},
    {"UnixDate", std::string(UnixDate), "Wed Feb  4 21:00:57 PST 2009"},
    {"RubyDate", std::string(RubyDate), "Wed Feb 04 21:00:57 -0800 2009"},
    {"RFC822", std::string(RFC822), "04 Feb 09 21:00 PST"},
    {"RFC850", std::string(RFC850), "Wednesday, 04-Feb-09 21:00:57 PST"},
    {"RFC1123", std::string(RFC1123), "Wed, 04 Feb 2009 21:00:57 PST"},
    {"RFC1123Z", std::string(RFC1123Z), "Wed, 04 Feb 2009 21:00:57 -0800"},
    {"RFC3339", std::string(RFC3339), "2009-02-04T21:00:57-08:00"},
    {"RFC3339Nano", std::string(RFC3339Nano), "2009-02-04T21:00:57.0123456-08:00"},
    {"Kitchen", std::string(Kitchen), "9:00PM"},
    {"am/pm", "3pm", "9pm"},
    {"AM/PM", "3PM", "9PM"},
    {"two-digit year", "06 01 02", "09 02 04"},
    // Three-letter months and days must not be followed by lower-case letter.
    {"Janet", "Hi Janet, the Month is January", "Hi Janet, the Month is February"},
    // Time stamps, Fractional seconds.
    {"Stamp", std::string(Stamp), "Feb  4 21:00:57"},
    {"StampMilli", std::string(StampMilli), "Feb  4 21:00:57.012"},
    {"StampMicro", std::string(StampMicro), "Feb  4 21:00:57.012345"},
    {"StampNano", std::string(StampNano), "Feb  4 21:00:57.012345600"},
    {"DateTime", std::string(DateTime), "2009-02-04 21:00:57"},
    {"DateOnly", std::string(DateOnly), "2009-02-04"},
    {"TimeOnly", std::string(TimeOnly), "21:00:57"},
    {"YearDay", "Jan  2 002 __2 2", "Feb  4 035  35 4"},
    {"Year", "2006 6 06 _6 __6 ___6", "2009 6 09 _6 __6 ___6"},
    {"Month", "Jan January 1 01 _1", "Feb February 2 02 _2"},
    {"DayOfMonth", "2 02 _2 __2", "4 04  4  35"},
    {"DayOfWeek", "Mon Monday", "Wed Wednesday"},
    {"Hour", "15 3 03 _3", "21 9 09 _9"},
    {"Minute", "4 04 _4", "0 00 _0"},
    {"Second", "5 05 _5", "57 57 _57"},
};

}  // namespace

TEST_F(GoTimeFormat, Format) {
    // The numeric time represents Thu Feb  4 21:00:57.012345600 PST 2009
    Time t = Unix(0, 1233810057012345600);
    for (const auto& test : formatTests) {
        EXPECT_EQ(S(t.Format(test.format)), test.result) << test.name;
    }
}

TEST_F(GoTimeFormat, GoString) {
    struct {
        Time in;
        std::string want;
    } tests[] = {
        {Date(2009, February, 5, 5, 0, 57, 12345600, UTC),
         "time.Date(2009, time.February, 5, 5, 0, 57, 12345600, time.UTC)"},
        {Date(2009, February, 5, 5, 0, 57, 12345600, Local),
         "time.Date(2009, time.February, 5, 5, 0, 57, 12345600, time.Local)"},
        {Date(2009, February, 5, 5, 0, 57, 12345600, FixedZone("Europe/Berlin", 3 * 60 * 60)),
         R"(time.Date(2009, time.February, 5, 5, 0, 57, 12345600, time.Location("Europe/Berlin")))"},
        {Date(2009, February, 5, 5, 0, 57, 12345600, FixedZone("Non-ASCII character ⏰", 3 * 60 * 60)),
         R"(time.Date(2009, time.February, 5, 5, 0, 57, 12345600, time.Location("Non-ASCII character \xe2\x8f\xb0")))"},
    };
    for (const auto& tt : tests) EXPECT_EQ(S(tt.in.GoString()), tt.want);
}

// issue 12440.
TEST_F(GoTimeFormat, FormatSingleDigits) {
    Time t = Date(2001, Month(2), 3, 4, 5, 6, 700000000, UTC);
    EXPECT_EQ(S(t.Format("3:4:5")), "4:5:6") << "single digit format";
}

TEST_F(GoTimeFormat, FormatShortYear) {
    int years[] = {
        -100001, -100000, -99999, -10001, -10000, -9999, -1001, -1000, -999, -101, -100, -99,
        -11,     -10,     -9,     -1,     0,      1,     9,     10,    11,   99,   100,  101,
        999,     1000,    1001,   9999,   10000,  10001, 99999, 100000, 100001,
    };
    for (int y : years) {
        Time t = Date(y, January, 1, 0, 0, 0, 0, UTC);
        std::string want;
        // The 4 in %04d counts the - sign, so print -y instead and
        // introduce our own - sign.
        if (y < 0) want = std::format("-{:04d}.{:02d}.{:02d}", -y, 1, 1);
        else want = std::format("{:04d}.{:02d}.{:02d}", y, 1, 1);
        EXPECT_EQ(S(t.Format("2006.01.02")), want) << "jan 1 " << y;
    }
}

namespace {

struct ParseTest {
    std::string name;
    std::string format;
    std::string value;
    bool hasTZ;      // contains a time zone
    bool hasWD;      // contains a weekday
    int yearSign;    // sign of year, -1 indicates the year is not present in the format
    int fracDigits;  // number of digits of fractional second
};

const std::vector<ParseTest> parseTests = {
    {"ANSIC", std::string(ANSIC), "Thu Feb  4 21:00:57 2010", false, true, 1, 0},
    {"UnixDate", std::string(UnixDate), "Thu Feb  4 21:00:57 PST 2010", true, true, 1, 0},
    {"RubyDate", std::string(RubyDate), "Thu Feb 04 21:00:57 -0800 2010", true, true, 1, 0},
    {"RFC850", std::string(RFC850), "Thursday, 04-Feb-10 21:00:57 PST", true, true, 1, 0},
    {"RFC1123", std::string(RFC1123), "Thu, 04 Feb 2010 21:00:57 PST", true, true, 1, 0},
    {"RFC1123", std::string(RFC1123), "Thu, 04 Feb 2010 22:00:57 PDT", true, true, 1, 0},
    {"RFC1123Z", std::string(RFC1123Z), "Thu, 04 Feb 2010 21:00:57 -0800", true, true, 1, 0},
    {"RFC3339", std::string(RFC3339), "2010-02-04T21:00:57-08:00", true, false, 1, 0},
    {"custom: \"2006-01-02 15:04:05-07\"", "2006-01-02 15:04:05-07", "2010-02-04 21:00:57-08", true, false, 1, 0},
    // Optional fractional seconds.
    {"ANSIC", std::string(ANSIC), "Thu Feb  4 21:00:57.0 2010", false, true, 1, 1},
    {"UnixDate", std::string(UnixDate), "Thu Feb  4 21:00:57.01 PST 2010", true, true, 1, 2},
    {"RubyDate", std::string(RubyDate), "Thu Feb 04 21:00:57.012 -0800 2010", true, true, 1, 3},
    {"RFC850", std::string(RFC850), "Thursday, 04-Feb-10 21:00:57.0123 PST", true, true, 1, 4},
    {"RFC1123", std::string(RFC1123), "Thu, 04 Feb 2010 21:00:57.01234 PST", true, true, 1, 5},
    {"RFC1123Z", std::string(RFC1123Z), "Thu, 04 Feb 2010 21:00:57.01234 -0800", true, true, 1, 5},
    {"RFC3339", std::string(RFC3339), "2010-02-04T21:00:57.012345678-08:00", true, false, 1, 9},
    {"custom: \"2006-01-02 15:04:05\"", "2006-01-02 15:04:05", "2010-02-04 21:00:57.0", false, false, 1, 0},
    // Amount of white space should not matter.
    {"ANSIC", std::string(ANSIC), "Thu Feb 4 21:00:57 2010", false, true, 1, 0},
    {"ANSIC", std::string(ANSIC), "Thu      Feb     4     21:00:57     2010", false, true, 1, 0},
    // Case should not matter
    {"ANSIC", std::string(ANSIC), "THU FEB 4 21:00:57 2010", false, true, 1, 0},
    {"ANSIC", std::string(ANSIC), "thu feb 4 21:00:57 2010", false, true, 1, 0},
    // Fractional seconds.
    {"millisecond:: dot separator", "Mon Jan _2 15:04:05.000 2006", "Thu Feb  4 21:00:57.012 2010", false, true, 1, 3},
    {"microsecond:: dot separator", "Mon Jan _2 15:04:05.000000 2006", "Thu Feb  4 21:00:57.012345 2010", false, true, 1, 6},
    {"nanosecond:: dot separator", "Mon Jan _2 15:04:05.000000000 2006", "Thu Feb  4 21:00:57.012345678 2010", false, true, 1, 9},
    {"millisecond:: comma separator", "Mon Jan _2 15:04:05,000 2006", "Thu Feb  4 21:00:57.012 2010", false, true, 1, 3},
    {"microsecond:: comma separator", "Mon Jan _2 15:04:05,000000 2006", "Thu Feb  4 21:00:57.012345 2010", false, true, 1, 6},
    {"nanosecond:: comma separator", "Mon Jan _2 15:04:05,000000000 2006", "Thu Feb  4 21:00:57.012345678 2010", false, true, 1, 9},

    // Leading zeros in other places should not be taken as fractional seconds.
    {"zero1", "2006.01.02.15.04.05.0", "2010.02.04.21.00.57.0", false, false, 1, 1},
    {"zero2", "2006.01.02.15.04.05.00", "2010.02.04.21.00.57.01", false, false, 1, 2},
    // Month and day names only match when not followed by a lower-case letter.
    {"Janet", "Hi Janet, the Month is January: Jan _2 15:04:05 2006", "Hi Janet, the Month is February: Feb  4 21:00:57 2010", false, true, 1, 0},

    // GMT with offset.
    {"GMT-8", std::string(UnixDate), "Fri Feb  5 05:00:57 GMT-8 2010", true, true, 1, 0},

    // Accept any number of fractional second digits (including none) for .999...
    // In Go 1, .999... was completely ignored in the format, meaning the first two
    // cases would succeed, but the next four would not. Go 1.1 accepts all six.
    // decimal "." separator.
    {"", "2006-01-02 15:04:05.9999 -0700 MST", "2010-02-04 21:00:57 -0800 PST", true, false, 1, 0},
    {"", "2006-01-02 15:04:05.999999999 -0700 MST", "2010-02-04 21:00:57 -0800 PST", true, false, 1, 0},
    {"", "2006-01-02 15:04:05.9999 -0700 MST", "2010-02-04 21:00:57.0123 -0800 PST", true, false, 1, 4},
    {"", "2006-01-02 15:04:05.999999999 -0700 MST", "2010-02-04 21:00:57.0123 -0800 PST", true, false, 1, 4},
    {"", "2006-01-02 15:04:05.9999 -0700 MST", "2010-02-04 21:00:57.012345678 -0800 PST", true, false, 1, 9},
    {"", "2006-01-02 15:04:05.999999999 -0700 MST", "2010-02-04 21:00:57.012345678 -0800 PST", true, false, 1, 9},
    // comma "," separator.
    {"", "2006-01-02 15:04:05,9999 -0700 MST", "2010-02-04 21:00:57 -0800 PST", true, false, 1, 0},
    {"", "2006-01-02 15:04:05,999999999 -0700 MST", "2010-02-04 21:00:57 -0800 PST", true, false, 1, 0},
    {"", "2006-01-02 15:04:05,9999 -0700 MST", "2010-02-04 21:00:57.0123 -0800 PST", true, false, 1, 4},
    {"", "2006-01-02 15:04:05,999999999 -0700 MST", "2010-02-04 21:00:57.0123 -0800 PST", true, false, 1, 4},
    {"", "2006-01-02 15:04:05,9999 -0700 MST", "2010-02-04 21:00:57.012345678 -0800 PST", true, false, 1, 9},
    {"", "2006-01-02 15:04:05,999999999 -0700 MST", "2010-02-04 21:00:57.012345678 -0800 PST", true, false, 1, 9},

    // issue 4502.
    {"", std::string(StampNano), "Feb  4 21:00:57.012345678", false, false, -1, 9},
    {"", "Jan _2 15:04:05.999", "Feb  4 21:00:57.012300000", false, false, -1, 4},
    {"", "Jan _2 15:04:05.999", "Feb  4 21:00:57.012345678", false, false, -1, 9},
    {"", "Jan _2 15:04:05.999999999", "Feb  4 21:00:57.0123", false, false, -1, 4},
    {"", "Jan _2 15:04:05.999999999", "Feb  4 21:00:57.012345678", false, false, -1, 9},

    // Day of year.
    {"", "2006-01-02 002 15:04:05", "2010-02-04 035 21:00:57", false, false, 1, 0},
    {"", "2006-01 002 15:04:05", "2010-02 035 21:00:57", false, false, 1, 0},
    {"", "2006-002 15:04:05", "2010-035 21:00:57", false, false, 1, 0},
    {"", "200600201 15:04:05", "201003502 21:00:57", false, false, 1, 0},
    {"", "200600204 15:04:05", "201003504 21:00:57", false, false, 1, 0},

    // Time zone offsets
    {"", "2006-01-02T15:04:05Z07", "2010-02-04T21:00:57Z", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07", "2010-02-04T21:00:57+08", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07", "2010-02-04T21:00:57-08", true, false, 1, 0},
    {"", "2006-01-02T15:04:05Z0700", "2010-02-04T21:00:57Z", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z0700", "2010-02-04T21:00:57+0800", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z0700", "2010-02-04T21:00:57-0800", true, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07:00", "2010-02-04T21:00:57Z", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07:00", "2010-02-04T21:00:57+08:00", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07:00", "2010-02-04T21:00:57-08:00", true, false, 1, 0},
    {"", "2006-01-02T15:04:05Z070000", "2010-02-04T21:00:57Z", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z070000", "2010-02-04T21:00:57+080000", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z070000", "2010-02-04T21:00:57-080000", true, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07:00:00", "2010-02-04T21:00:57Z", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07:00:00", "2010-02-04T21:00:57+08:00:00", false, false, 1, 0},
    {"", "2006-01-02T15:04:05Z07:00:00", "2010-02-04T21:00:57-08:00:00", true, false, 1, 0},
};

void checkTime(const Time& t, const ParseTest& test) {
    // The time should be Thu Feb  4 21:00:57 PST 2010
    if (test.yearSign >= 0) { EXPECT_EQ(test.yearSign * t.Year(), 2010) << test.name << ": bad year"; }
    EXPECT_EQ(t.Month(), February) << test.name << ": bad month";
    EXPECT_EQ(t.Day(), 4) << test.name << ": bad day";
    EXPECT_EQ(t.Hour(), 21) << test.name << ": bad hour";
    EXPECT_EQ(t.Minute(), 0) << test.name << ": bad minute";
    EXPECT_EQ(t.Second(), 57) << test.name << ": bad second";
    // Nanoseconds must be checked against the precision of the input.
    int nanosec = std::stoi(std::string("012345678").substr(0, test.fracDigits) +
                            std::string("000000000").substr(0, 9 - test.fracDigits));
    EXPECT_EQ(t.Nanosecond(), nanosec) << test.name << ": bad nanosecond";
    auto [name, offset] = t.Zone();
    if (test.hasTZ) { EXPECT_EQ(offset, -28800) << test.name << ": bad tz offset: " << name.view(); }
    if (test.hasWD) { EXPECT_EQ(t.Weekday(), Thursday) << test.name << ": bad weekday"; }
}

}  // namespace

TEST_F(GoTimeFormat, Parse) {
    for (const auto& test : parseTests) {
        auto [t, err] = Parse(test.format, test.value);
        if (err) {
            ADD_FAILURE() << test.name << " error: " << err.Error().view();
            continue;
        }
        checkTime(t, test);
    }
}

TEST_F(GoTimeFormat, ParseDayOutOfRange) {
    // All parsed with ANSIC.
    struct {
        std::string date;
        bool ok;
    } tests[] = {
        {"Thu Jan 99 21:00:57 2010", false}, {"Thu Jan 31 21:00:57 2010", true},
        {"Thu Jan 32 21:00:57 2010", false}, {"Thu Feb 28 21:00:57 2012", true},
        {"Thu Feb 29 21:00:57 2012", true},  {"Thu Feb 29 21:00:57 2010", false},
        {"Thu Mar 31 21:00:57 2010", true},  {"Thu Mar 32 21:00:57 2010", false},
        {"Thu Apr 30 21:00:57 2010", true},  {"Thu Apr 31 21:00:57 2010", false},
        {"Thu May 31 21:00:57 2010", true},  {"Thu May 32 21:00:57 2010", false},
        {"Thu Jun 30 21:00:57 2010", true},  {"Thu Jun 31 21:00:57 2010", false},
        {"Thu Jul 31 21:00:57 2010", true},  {"Thu Jul 32 21:00:57 2010", false},
        {"Thu Aug 31 21:00:57 2010", true},  {"Thu Aug 32 21:00:57 2010", false},
        {"Thu Sep 30 21:00:57 2010", true},  {"Thu Sep 31 21:00:57 2010", false},
        {"Thu Oct 31 21:00:57 2010", true},  {"Thu Oct 32 21:00:57 2010", false},
        {"Thu Nov 30 21:00:57 2010", true},  {"Thu Nov 31 21:00:57 2010", false},
        {"Thu Dec 31 21:00:57 2010", true},  {"Thu Dec 32 21:00:57 2010", false},
        {"Thu Dec 00 21:00:57 2010", false},
    };
    for (const auto& test : tests) {
        auto [_, err] = Parse(ANSIC, test.date);
        if (test.ok) {
            EXPECT_FALSE(err) << test.date << ": unexpected error: " << (err ? err.Error().view() : "");
        } else {
            ASSERT_TRUE(err) << test.date << ": expected 'day' error, got none";
            EXPECT_NE(err.Error().view().find("day out of range"), std::string_view::npos) << err.Error().view();
        }
    }
}

// TestParseInLocation checks that the Parse and ParseInLocation functions
// do not get confused by the fact that AST (Arabia Standard Time) and AST
// (Atlantic Standard Time) are different time zones, even though they have
// the same abbreviation.
TEST_F(GoTimeFormat, ParseInLocation) {
    auto [baghdad, err] = LoadLocation("Asia/Baghdad");
    ASSERT_FALSE(err) << err.Error().view();

    auto [t1, err1] = ParseInLocation("Jan 02 2006 MST", "Feb 01 2013 AST", baghdad);
    ASSERT_FALSE(err1) << err1.Error().view();
    auto [_, offset] = t1.Zone();
    // A zero offset means that ParseInLocation did not recognize the 'AST'
    // abbreviation as matching the current location (Baghdad, where we'd
    // expect a +03 hrs offset); likely because we're using a recent tzdata
    // release (2017a or newer). If it happens, skip the Baghdad test.
    if (offset != 0) {
        Time t2 = Date(2013, February, 1, 00, 00, 00, 0, baghdad);
        EXPECT_TRUE(identical(t1, t2)) << t1.String().view() << " want " << t2.String().view();
        EXPECT_EQ(offset, 3 * 60 * 60);
    }

    auto [blancSablon, err2] = LoadLocation("America/Blanc-Sablon");
    ASSERT_FALSE(err2) << err2.Error().view();
    // In this case 'AST' means 'Atlantic Standard Time', and we expect the
    // abbreviation to correctly match the american location.
    auto [t3, err3] = ParseInLocation("Jan 02 2006 MST", "Feb 01 2013 AST", blancSablon);
    ASSERT_FALSE(err3) << err3.Error().view();
    Time t4 = Date(2013, February, 1, 00, 00, 00, 0, blancSablon);
    EXPECT_TRUE(identical(t3, t4)) << t3.String().view() << " want " << t4.String().view();
    EXPECT_EQ(t3.Zone().second, -4 * 60 * 60);
}

// Problematic time zone format needs special tests.
TEST_F(GoTimeFormat, RubyParse) {
    std::vector<ParseTest> rubyTests = {
        {"RubyDate", std::string(RubyDate), "Thu Feb 04 21:00:57 -0800 2010", true, true, 1, 0},
        // Ignore the time zone in the test. If it parses, it'll be OK.
        {"RubyDate", std::string(RubyDate), "Thu Feb 04 21:00:57 -0000 2010", false, true, 1, 0},
        {"RubyDate", std::string(RubyDate), "Thu Feb 04 21:00:57 +0000 2010", false, true, 1, 0},
        {"RubyDate", std::string(RubyDate), "Thu Feb 04 21:00:57 +1130 2010", false, true, 1, 0},
    };
    for (const auto& test : rubyTests) {
        auto [t, err] = Parse(test.format, test.value);
        if (err) {
            ADD_FAILURE() << test.name << " error: " << err.Error().view();
            continue;
        }
        checkTime(t, test);
    }
}

TEST_F(GoTimeFormat, FormatAndParse) {
    const std::string format = "Mon MST " + std::string(RFC3339);  // all fields
    auto f = [&](int64 sec) {
        Time t1 = Unix(sec / 2, 0);
        // not required to work
        if (t1.Year() < 1000 || t1.Year() > 9999 || t1.Unix() != sec) return true;
        auto [t2, err] = Parse(format, t1.Format(format));
        if (err) {
            ADD_FAILURE() << "error: " << err.Error().view();
            return false;
        }
        if (t1.Unix() != t2.Unix() || t1.Nanosecond() != t2.Nanosecond()) {
            ADD_FAILURE() << "FormatAndParse " << sec << ": " << t1.String().view() << " " << t2.String().view();
            return false;
        }
        return true;
    };
    // Try a reasonable date first, then the huge ones.
    std::mt19937_64 rng(1);
    for (int i = 0; i < 10000; i++) {
        if (!f(static_cast<int32>(rng()))) break;
    }
    for (int i = 0; i < 10000; i++) {
        if (!f(static_cast<int64>(rng()))) break;
    }
}

TEST_F(GoTimeFormat, ParseTimeZone) {
    struct {
        std::string value;
        int length;
        bool ok;
    } tests[] = {
        {"gmt hi there", 0, false},
        {"GMT hi there", 3, true},
        {"GMT+12 hi there", 6, true},
        {"GMT+00 hi there", 6, true},
        {"GMT+", 3, true},
        {"GMT+3", 5, true},
        {"GMT+a", 3, true},
        {"GMT+3a", 5, true},
        {"GMT-5 hi there", 5, true},
        {"GMT-51 hi there", 3, true},
        {"ChST hi there", 4, true},
        {"MeST hi there", 4, true},
        {"MSDx", 3, true},
        {"MSDY", 0, false},  // four letters must end in T.
        {"ESAST hi", 5, true},
        {"ESASTT hi", 0, false},  // run of upper-case letters too long.
        {"ESATY hi", 0, false},   // five letters must end in T.
        {"WITA hi", 4, true},     // Issue #18251
        // Issue #24071
        {"+03 hi", 3, true},
        {"-04 hi", 3, true},
        // Issue #26032
        {"+00", 3, true},
        {"-11", 3, true},
        {"-12", 3, true},
        {"-23", 3, true},
        {"-24", 0, false},
        {"+13", 3, true},
        {"+14", 3, true},
        {"+23", 3, true},
        {"+24", 0, false},
    };
    for (const auto& test : tests) {
        auto [length, ok] = hooks::ParseTimeZone(test.value);
        EXPECT_EQ(ok, test.ok) << test.value;
        if (ok == test.ok) { EXPECT_EQ(length, test.length) << test.value; }
    }
}

TEST_F(GoTimeFormat, ParseErrors) {
    struct {
        std::string format;
        std::string value;
        std::string expect;  // must appear within the error
    } tests[] = {
        {std::string(ANSIC), "Feb  4 21:00:60 2010", R"(cannot parse "Feb  4 21:00:60 2010" as "Mon")"},
        {std::string(ANSIC), "Thu Feb  4 21:00:57 @2010", R"(cannot parse "@2010" as "2006")"},
        {std::string(ANSIC), "Thu Feb  4 21:00:60 2010", "second out of range"},
        {std::string(ANSIC), "Thu Feb  4 21:61:57 2010", "minute out of range"},
        {std::string(ANSIC), "Thu Feb  4 24:00:60 2010", "hour out of range"},
        {"Mon Jan _2 15:04:05.000 2006", "Thu Feb  4 23:00:59x01 2010", R"(cannot parse "x01 2010" as ".000")"},
        {"Mon Jan _2 15:04:05.000 2006", "Thu Feb  4 23:00:59.xxx 2010", R"(cannot parse ".xxx 2010" as ".000")"},
        {"Mon Jan _2 15:04:05.000 2006", "Thu Feb  4 23:00:59.-123 2010", "fractional second out of range"},
        // issue 4502. StampNano requires exactly 9 digits of precision.
        {std::string(StampNano), "Dec  7 11:22:01.000000", R"(cannot parse ".000000" as ".000000000")"},
        {std::string(StampNano), "Dec  7 11:22:01.0000000000", R"(extra text: "0")"},
        // issue 4493. Helpful errors.
        {std::string(RFC3339), "2006-01-02T15:04:05Z07:00", R"(parsing time "2006-01-02T15:04:05Z07:00": extra text: "07:00")"},
        {std::string(RFC3339), "2006-01-02T15:04_abc", R"(parsing time "2006-01-02T15:04_abc" as "2006-01-02T15:04:05Z07:00": cannot parse "_abc" as ":")"},
        {std::string(RFC3339), "2006-01-02T15:04:05_abc", R"(parsing time "2006-01-02T15:04:05_abc" as "2006-01-02T15:04:05Z07:00": cannot parse "_abc" as "Z07:00")"},
        {std::string(RFC3339), "2006-01-02T15:04:05Z_abc", R"(parsing time "2006-01-02T15:04:05Z_abc": extra text: "_abc")"},
        // invalid second followed by optional fractional seconds
        {std::string(RFC3339), "2010-02-04T21:00:67.012345678-08:00", "second out of range"},
        // issue 54569
        {std::string(RFC3339), "0000-01-01T00:00:.0+00:00", R"(parsing time "0000-01-01T00:00:.0+00:00" as "2006-01-02T15:04:05Z07:00": cannot parse ".0+00:00" as "05")"},
        // issue 21113
        {"_2 Jan 06 15:04 MST", "4 --- 00 00:00 GMT", R"(cannot parse "--- 00 00:00 GMT" as "Jan")"},
        {"_2 January 06 15:04 MST", "4 --- 00 00:00 GMT", R"(cannot parse "--- 00 00:00 GMT" as "January")"},

        // invalid or mismatched day-of-year
        {"Jan _2 002 2006", "Feb  4 034 2006", "day-of-year does not match day"},
        {"Jan _2 002 2006", "Feb  4 004 2006", "day-of-year does not match month"},

        // issue 45391.
        {R"("2006-01-02T15:04:05Z07:00")", "0", R"(parsing time "0" as "\"2006-01-02T15:04:05Z07:00\"": cannot parse "0" as "\"")"},
        {std::string(RFC3339), "\"", R"(parsing time "\"" as "2006-01-02T15:04:05Z07:00": cannot parse "\"" as "2006")"},

        // issue 54570
        {std::string(RFC3339), "0000-01-01T00:00:00+00:+0", R"(parsing time "0000-01-01T00:00:00+00:+0" as "2006-01-02T15:04:05Z07:00": cannot parse "+00:+0" as "Z07:00")"},
        {std::string(RFC3339), "0000-01-01T00:00:00+-0:00", R"(parsing time "0000-01-01T00:00:00+-0:00" as "2006-01-02T15:04:05Z07:00": cannot parse "+-0:00" as "Z07:00")"},

        // issue 56730
        {"2006-01-02", "22-10-25", R"(parsing time "22-10-25" as "2006-01-02": cannot parse "22-10-25" as "2006")"},
        {"06-01-02", "a2-10-25", R"(parsing time "a2-10-25" as "06-01-02": cannot parse "a2-10-25" as "06")"},
        {"03:04PM", "12:03pM", R"(parsing time "12:03pM" as "03:04PM": cannot parse "pM" as "PM")"},
        {"03:04pm", "12:03pM", R"(parsing time "12:03pM" as "03:04pm": cannot parse "pM" as "pm")"},

        // issue 67470
        {"-07", "-25", "time zone offset hour out of range"},
        {"-07:00", "+25:00", "time zone offset hour out of range"},
        {"-07:00", "-23:61", "time zone offset minute out of range"},
        {"-07:00:00", "+23:59:61", "time zone offset second out of range"},
        {"Z07", "-25", "time zone offset hour out of range"},
        {"Z07:00", "+25:00", "time zone offset hour out of range"},
        {"Z07:00", "-23:61", "time zone offset minute out of range"},
        {"Z07:00:00", "+23:59:61", "time zone offset second out of range"},
    };
    for (const auto& test : tests) {
        auto [_, err] = Parse(test.format, test.value);
        if (!err) {
            ADD_FAILURE() << "expected error for " << test.format << " " << test.value;
            continue;
        }
        EXPECT_NE(err.Error().view().find(test.expect), std::string_view::npos)
            << "expected error with " << test.expect << " for " << test.format << " " << test.value << "; got "
            << err.Error().view();
    }
}

TEST_F(GoTimeFormat, NoonIs12PM) {
    Time noon = Date(0, January, 1, 12, 0, 0, 0, UTC);
    EXPECT_EQ(S(noon.Format("3:04PM")), "12:00PM");
    EXPECT_EQ(S(noon.Format("03:04PM")), "12:00PM");
}

TEST_F(GoTimeFormat, MidnightIs12AM) {
    Time midnight = Date(0, January, 1, 0, 0, 0, 0, UTC);
    EXPECT_EQ(S(midnight.Format("3:04PM")), "12:00AM");
    EXPECT_EQ(S(midnight.Format("03:04PM")), "12:00AM");
}

TEST_F(GoTimeFormat, 12PMIsNoon) {
    auto [noon, err] = Parse("3:04PM", "12:00PM");
    ASSERT_FALSE(err);
    EXPECT_EQ(noon.Hour(), 12);
    auto [noon2, err2] = Parse("03:04PM", "12:00PM");
    ASSERT_FALSE(err2);
    EXPECT_EQ(noon2.Hour(), 12);
}

TEST_F(GoTimeFormat, 12AMIsMidnight) {
    auto [midnight, err] = Parse("3:04PM", "12:00AM");
    ASSERT_FALSE(err);
    EXPECT_EQ(midnight.Hour(), 0);
    auto [midnight2, err2] = Parse("03:04PM", "12:00AM");
    ASSERT_FALSE(err2);
    EXPECT_EQ(midnight2.Hour(), 0);
}

// Check that a time without a Zone still produces a (numeric) time zone
// when formatted with MST as a requested zone.
TEST_F(GoTimeFormat, MissingZone) {
    auto [t, err] = Parse(RubyDate, "Thu Feb 02 16:10:03 -0500 2006");
    ASSERT_FALSE(err);
    EXPECT_EQ(S(t.Format(UnixDate)), "Thu Feb  2 16:10:03 -0500 2006");  // -0500 not EST
}

TEST_F(GoTimeFormat, MinutesInTimeZone) {
    auto [t, err] = Parse(RubyDate, "Mon Jan 02 15:04:05 +0123 2006");
    ASSERT_FALSE(err);
    EXPECT_EQ(t.Zone().second, (1 * 60 + 23) * 60);
}

namespace {

struct SecondsTimeZoneOffsetTest {
    std::string format;
    std::string value;
    int expectedoffset;
};

const std::vector<SecondsTimeZoneOffsetTest> secondsTimeZoneOffsetTests = {
    {"2006-01-02T15:04:05-070000", "1871-01-01T05:33:02-003408", -(34 * 60 + 8)},
    {"2006-01-02T15:04:05-07:00:00", "1871-01-01T05:33:02-00:34:08", -(34 * 60 + 8)},
    {"2006-01-02T15:04:05-070000", "1871-01-01T05:33:02+003408", 34 * 60 + 8},
    {"2006-01-02T15:04:05-07:00:00", "1871-01-01T05:33:02+00:34:08", 34 * 60 + 8},
    {"2006-01-02T15:04:05Z070000", "1871-01-01T05:33:02-003408", -(34 * 60 + 8)},
    {"2006-01-02T15:04:05Z07:00:00", "1871-01-01T05:33:02+00:34:08", 34 * 60 + 8},
    {"2006-01-02T15:04:05-07", "1871-01-01T05:33:02+01", 1 * 60 * 60},
    {"2006-01-02T15:04:05-07", "1871-01-01T05:33:02-02", -2 * 60 * 60},
    {"2006-01-02T15:04:05Z07", "1871-01-01T05:33:02-02", -2 * 60 * 60},
};

}  // namespace

TEST_F(GoTimeFormat, ParseSecondsInTimeZone) {
    // should accept timezone offsets with seconds like: Zone
    // America/New_York   -4:56:02 -      LMT     1883 Nov 18 12:03:58
    for (const auto& test : secondsTimeZoneOffsetTests) {
        auto [t, err] = Parse(test.format, test.value);
        ASSERT_FALSE(err) << err.Error().view();
        EXPECT_EQ(t.Zone().second, test.expectedoffset);
    }
}

TEST_F(GoTimeFormat, FormatSecondsInTimeZone) {
    for (const auto& test : secondsTimeZoneOffsetTests) {
        Time d = Date(1871, Month(1), 1, 5, 33, 2, 0, FixedZone("LMT", test.expectedoffset));
        EXPECT_EQ(S(d.Format(test.format)), test.value);
    }
}

// Issue 11334.
TEST_F(GoTimeFormat, UnderscoreTwoThousand) {
    auto [t, err] = Parse("15:04_20060102", "14:38_20150618");
    EXPECT_FALSE(err);
    auto [y, m, d] = t.Date();
    EXPECT_TRUE(y == 2015 && m == Month(6) && d == 18) << y << "/" << m.value << "/" << d;
    EXPECT_EQ(t.Hour(), 14);
    EXPECT_EQ(t.Minute(), 38);
}

// Issue 29918, 29916
TEST_F(GoTimeFormat, Std0xParseError) {
    struct {
        std::string format, value, valueElemPrefix;
    } tests[] = {
        {"01 MST", "0 MST", "0"},
        {"01 MST", "1 MST", "1"},
        {std::string(RFC850), "Thursday, 04-Feb-1 21:00:57 PST", "1"},
    };
    for (const auto& tt : tests) {
        auto [_, err] = Parse(tt.format, tt.value);
        ASSERT_TRUE(err) << "Parse(" << tt.format << ", " << tt.value << ") did not fail as expected";
        auto* perr = errors::As<ParseError>(err);
        ASSERT_TRUE(perr) << "expected ParseError";
        EXPECT_NE(perr->Error().view().find("cannot parse"), std::string_view::npos);
        EXPECT_TRUE(perr->ValueElem.view().starts_with(tt.valueElemPrefix)) << perr->Error().view();
    }
}

TEST_F(GoTimeFormat, ParseMonthOutOfRange) {
    struct {
        std::string value;
        bool ok;
    } tests[] = {{"00-01", false}, {"13-01", false}, {"01-01", true}};
    for (const auto& test : tests) {
        auto [_, err] = Parse("01-02", test.value);
        if (test.ok) {
            EXPECT_FALSE(err) << test.value;
        } else {
            ASSERT_TRUE(err) << test.value << ": expected 'month' error, got none";
            EXPECT_NE(err.Error().view().find("month out of range"), std::string_view::npos) << err.Error().view();
        }
    }
}

// Issue 37387.
TEST_F(GoTimeFormat, ParseYday) {
    for (int i = 1; i <= 365; i++) {
        std::string d = std::format("2020-{:03d}", i);
        auto [tm, err] = Parse("2006-002", d);
        if (err) {
            ADD_FAILURE() << "unexpected error for " << d << ": " << err.Error().view();
            continue;
        }
        EXPECT_TRUE(tm.Year() == 2020 && tm.YearDay() == i) << "got year " << tm.Year() << " yearday " << tm.YearDay();
    }
}

// Issue 45391.
TEST_F(GoTimeFormat, Quote) {
    struct {
        std::string s, want;
    } tests[] = {
        {R"(")", R"("\"")"},
        {R"(abc"xyz")", R"("abc\"xyz\"")"},
        {"", R"("")"},
        {"abc", R"("abc")"},
        {"☺", R"("\xe2\x98\xba")"},
        {"☺ hello ☺ hello", R"("\xe2\x98\xba hello \xe2\x98\xba hello")"},
        {"\x04", R"("\x04")"},
    };
    for (const auto& tt : tests) EXPECT_EQ(quote(tt.s), tt.want) << tt.s;
}

// Issue 48037
TEST_F(GoTimeFormat, FormatFractionalSecondSeparators) {
    struct {
        std::string s, want;
    } tests[] = {
        {"15:04:05.000", "21:00:57.012"},
        {"15:04:05.999", "21:00:57.012"},
        {"15:04:05,000", "21:00:57,012"},
        {"15:04:05,999", "21:00:57,012"},
    };
    // The numeric time represents Thu Feb  4 21:00:57.012345600 PST 2009
    Time t = Unix(0, 1233810057012345600);
    for (const auto& tt : tests) EXPECT_EQ(S(t.Format(tt.s)), tt.want) << tt.s;
}

// Issue 48685 and 54567.
TEST_F(GoTimeFormat, ParseFractionalSecondsLongerThanNineDigits) {
    struct {
        std::string value;
        int want;
    } tests[] = {
        // 9 digits
        {"2021-09-29T16:04:33.000000000Z", 0},
        {"2021-09-29T16:04:33.000000001Z", 1},
        {"2021-09-29T16:04:33.100000000Z", 100'000'000},
        {"2021-09-29T16:04:33.100000001Z", 100'000'001},
        {"2021-09-29T16:04:33.999999999Z", 999'999'999},
        {"2021-09-29T16:04:33.012345678Z", 12'345'678},
        // 10 digits, truncates
        {"2021-09-29T16:04:33.0000000000Z", 0},
        {"2021-09-29T16:04:33.0000000001Z", 0},
        {"2021-09-29T16:04:33.1000000000Z", 100'000'000},
        {"2021-09-29T16:04:33.1000000009Z", 100'000'000},
        {"2021-09-29T16:04:33.9999999999Z", 999'999'999},
        {"2021-09-29T16:04:33.0123456789Z", 12'345'678},
        // 11 digits, truncates
        {"2021-09-29T16:04:33.10000000000Z", 100'000'000},
        {"2021-09-29T16:04:33.00123456789Z", 1'234'567},
        // 12 digits, truncates
        {"2021-09-29T16:04:33.000123456789Z", 123'456},
        // 15 digits, truncates
        {"2021-09-29T16:04:33.9999999999999999Z", 999'999'999},
    };
    for (const auto& tt : tests) {
        for (std::string_view format : {RFC3339, RFC3339Nano}) {
            auto [tm, err] = Parse(format, tt.value);
            if (err) {
                ADD_FAILURE() << "Parse(" << format << ", " << tt.value << ") error: " << err.Error().view();
                continue;
            }
            EXPECT_EQ(tm.Nanosecond(), tt.want) << "Parse(" << format << ", " << tt.value << ")";
        }
    }
}
