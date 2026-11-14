// Ported from Go 1.27.3's time/zoneinfo_test.go and
// time/zoneinfo_unix_test.go, with Go's test names.
//
// Go's tests read zones from the zip file in its tree, with the platform
// sources disabled; these read the system's zoneinfo. testdata/ is
// go_time_testdata.hpp.
//
// Left out: TestEnvVarUsage (go++ reads ZONEINFO on each LoadLocation,
// where Go caches it). TestLoadLocationFromTZData reads the system's file,
// where Go reads its zip file, and also checks that the two calls share
// their Location, as go++'s do.
#include "go_time_testdata.hpp"
#include "time/internal.hpp"

#include <gopp/errors.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>

#include <sys/stat.h>

using namespace gopp;
namespace hooks = gopp::time::testing_hooks;

namespace {

class GoTimeZoneinfo : public ::testing::Test {
protected:
    static void SetUpTestSuite() { hooks::ForceUSPacificForTesting(); }
    static void TearDownTestSuite() { hooks::ResetLocalOnceForTest(); }
};

std::string S(const string& s) { return std::string(s.view()); }
std::string S(const error& e) { return e ? std::string(e.Error().view()) : "<nil>"; }

// t.Setenv: sets an environment variable for the rest of the test.
class scopedEnv {
public:
    scopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old, had_ = true;
        if (value) ::setenv(name, value, 1);
        else ::unsetenv(name);
    }
    ~scopedEnv() {
        if (had_) ::setenv(name_, old_.c_str(), 1);
        else ::unsetenv(name_);
    }

private:
    const char* name_;
    std::string old_;
    bool had_ = false;
};

bool exists(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0;
}

}  // namespace

TEST_F(GoTimeZoneinfo, BadLocationErrMsg) {
    scopedEnv env("ZONEINFO", nullptr);
    std::string loc = "Asia/SomethingNotExist";
    auto [_, err] = time::LoadLocation(loc);
    EXPECT_EQ(S(err), "unknown time zone " + loc);
}

TEST_F(GoTimeZoneinfo, LoadLocationValidatesNames) {
    scopedEnv env("ZONEINFO", "");
    const char* bad[] = {
        "/usr/foo/Foo",
        "\\UNC\foo",
        "..",
        "a..",
    };
    for (const char* v : bad) {
        auto [_, err] = time::LoadLocation(v);
        EXPECT_EQ(err, hooks::ErrLocation) << "LoadLocation(" << v << ") error = " << S(err) << "; want ErrLocation";
    }
}

TEST_F(GoTimeZoneinfo, Version3) {
    auto [_, err] = time::LoadLocation("Asia/Jerusalem");
    ASSERT_FALSE(err) << S(err);
}

// Test that we get the correct results for times before the first
// transition time. To do this we explicitly check early dates in a couple
// of specific timezones.
TEST_F(GoTimeZoneinfo, FirstZone) {
    const std::string format = "Mon, 02 Jan 2006 15:04:05 -0700 (MST)";
    struct {
        std::string zone;
        int64 unix;
        std::string want1;
        std::string want2;
    } tests[] = {
        {"PST8PDT", -1633269601, "Sun, 31 Mar 1918 01:59:59 -0800 (PST)", "Sun, 31 Mar 1918 03:00:00 -0700 (PDT)"},
        {"Pacific/Fakaofo", 1325242799, "Thu, 29 Dec 2011 23:59:59 -1100 (-11)", "Sat, 31 Dec 2011 00:00:00 +1300 (+13)"},
    };
    for (const auto& test : tests) {
        auto [z, err] = time::LoadLocation(test.zone);
        // Systems that split off tzdata-legacy lack PST8PDT, which tzdata
        // 2024b made a link to America/Los_Angeles.
        if (err && test.zone == "PST8PDT") std::tie(z, err) = time::LoadLocation("America/Los_Angeles");
        ASSERT_FALSE(err) << S(err);
        EXPECT_EQ(S(time::Unix(test.unix, 0).In(z).Format(format)), test.want1) << test.zone << " " << test.unix;
        EXPECT_EQ(S(time::Unix(test.unix + 1, 0).In(z).Format(format)), test.want2) << test.zone << " " << test.unix;
    }
}

TEST_F(GoTimeZoneinfo, LocationNames) {
    EXPECT_EQ(S(time::Local->String()), "Local");
    EXPECT_EQ(S(time::UTC->String()), "UTC");
}

TEST_F(GoTimeZoneinfo, LoadLocationFromTZData) {
    const std::string locationName = "Asia/Jerusalem";
    auto [reference, err] = time::LoadLocation(locationName);
    ASSERT_FALSE(err) << S(err);

    std::ifstream f("/usr/share/zoneinfo/" + locationName, std::ios::binary);
    ASSERT_TRUE(f) << "no /usr/share/zoneinfo/" << locationName;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string tzinfo = ss.str();
    auto [sample, err2] = time::LoadLocationFromTZData(locationName, as_bytes(tzinfo));
    ASSERT_FALSE(err2) << S(err2);

    // Go compares them with reflect.DeepEqual; go++ shares them.
    EXPECT_EQ(reference, sample) << "return values of LoadLocationFromTZData and LoadLocation don't match";
}

// Issue 30099.
TEST_F(GoTimeZoneinfo, EarlyLocation) {
    const std::string locName = "America/New_York";
    auto [loc, err] = time::LoadLocation(locName);
    ASSERT_FALSE(err) << S(err);

    time::Time d = time::Date(1900, time::January, 1, 0, 0, 0, 0, loc);
    auto [tzName, tzOffset] = d.Zone();
    EXPECT_EQ(S(tzName), "EST");
    EXPECT_EQ(tzOffset, -18000);
}

TEST_F(GoTimeZoneinfo, MalformedTZData) {
    // The goal here is just that malformed tzdata results in an error, not
    // a panic.
    std::string issue29437("TZif\x00" "000000000000000\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" "00", 46);
    auto [_, err] = time::LoadLocationFromTZData("abc", as_bytes(issue29437));
    EXPECT_TRUE(err) << "expected error, got none";
}

TEST_F(GoTimeZoneinfo, LoadLocationFromTZDataSlim) {
    struct {
        std::string zoneName;
        std::string fileName;
        std::function<time::Time(const time::Location*)> date;
        std::string wantName;
        int wantOffset;
    } slimTests[] = {
        {"Europe/Berlin", "2020b_Europe_Berlin",
         [](const time::Location* loc) { return time::Date(2020, time::October, 29, 15, 30, 0, 0, loc); }, "CET", 3600},
        {"America/Nuuk", "2021a_America_Nuuk",
         [](const time::Location* loc) { return time::Date(2020, time::October, 29, 15, 30, 0, 0, loc); }, "-03",
         -10800},
        {"Asia/Gaza", "2021a_Asia_Gaza",
         [](const time::Location* loc) { return time::Date(2020, time::October, 29, 15, 30, 0, 0, loc); }, "EET", 7200},
        {"Europe/Dublin", "2021a_Europe_Dublin",
         [](const time::Location* loc) { return time::Date(2021, time::April, 2, 11, 12, 13, 0, loc); }, "IST", 3600},
    };
    for (const auto& test : slimTests) {
        const std::string& tzData = go_time_testdata::files().at(test.fileName);
        auto [reference, err] = time::LoadLocationFromTZData(test.zoneName, as_bytes(tzData));
        if (err) {
            ADD_FAILURE() << S(err);
            continue;
        }
        time::Time d = test.date(reference);
        auto [tzName, tzOffset] = d.Zone();
        EXPECT_EQ(S(tzName), test.wantName) << test.zoneName;
        EXPECT_EQ(tzOffset, test.wantOffset) << test.zoneName;
    }
}

TEST_F(GoTimeZoneinfo, Tzset) {
    struct {
        std::string inStr;
        int64 inEnd;
        int64 inSec;
        std::string name;
        int off;
        int64 start;
        int64 end;
        bool isDST;
        bool ok;
    } tests[] = {
        {"", 0, 0, "", 0, 0, 0, false, false},
        {"PST8PDT,M3.2.0,M11.1.0", 0, 2159200800, "PDT", -7 * 60 * 60, 2152173600, 2172733200, true, true},
        {"PST8PDT,M3.2.0,M11.1.0", 0, 2152173599, "PST", -8 * 60 * 60, 2145916800, 2152173600, false, true},
        {"PST8PDT,M3.2.0,M11.1.0", 0, 2152173600, "PDT", -7 * 60 * 60, 2152173600, 2172733200, true, true},
        {"PST8PDT,M3.2.0,M11.1.0", 0, 2152173601, "PDT", -7 * 60 * 60, 2152173600, 2172733200, true, true},
        {"PST8PDT,M3.2.0,M11.1.0", 0, 2172733199, "PDT", -7 * 60 * 60, 2152173600, 2172733200, true, true},
        {"PST8PDT,M3.2.0,M11.1.0", 0, 2172733200, "PST", -8 * 60 * 60, 2172733200, 2177452800, false, true},
        {"PST8PDT,M3.2.0,M11.1.0", 0, 2172733201, "PST", -8 * 60 * 60, 2172733200, 2177452800, false, true},
        {"KST-9", 592333200, 1677246697, "KST", 9 * 60 * 60, 592333200, INT64_MAX, false, true},
    };
    for (const auto& test : tests) {
        auto r = hooks::Tzset(test.inStr, test.inEnd, test.inSec);
        EXPECT_TRUE(r.name == test.name && r.offset == test.off && r.start == test.start && r.end == test.end &&
                    r.isDST == test.isDST && r.ok == test.ok)
            << "tzset(" << test.inStr << ", " << test.inEnd << ", " << test.inSec << ") = " << r.name << ", "
            << r.offset << ", " << r.start << ", " << r.end << ", " << r.isDST << ", " << r.ok;
    }
}

TEST_F(GoTimeZoneinfo, TzsetName) {
    struct {
        std::string in, name, out;
        bool ok;
    } tests[] = {
        {"", "", "", false},
        {"X", "", "", false},
        {"PST", "PST", "", true},
        {"PST8PDT", "PST", "8PDT", true},
        {"PST-08", "PST", "-08", true},
        {"<A+B>+08", "A+B", "+08", true},
    };
    for (const auto& test : tests) {
        auto [name, out, ok] = hooks::TzsetName(test.in);
        EXPECT_TRUE(name == test.name && out == test.out && ok == test.ok)
            << "tzsetName(" << test.in << ") = " << name << ", " << out << ", " << ok;
    }
}

TEST_F(GoTimeZoneinfo, TzsetOffset) {
    struct {
        std::string in;
        int off;
        std::string out;
        bool ok;
    } tests[] = {
        {"", 0, "", false},
        {"X", 0, "", false},
        {"+", 0, "", false},
        {"+08", 8 * 60 * 60, "", true},
        {"-01:02:03", -1 * 60 * 60 - 2 * 60 - 3, "", true},
        {"01", 1 * 60 * 60, "", true},
        {"100", 100 * 60 * 60, "", true},
        {"1000", 0, "", false},
        {"8PDT", 8 * 60 * 60, "PDT", true},
    };
    for (const auto& test : tests) {
        auto [off, out, ok] = hooks::TzsetOffset(test.in);
        EXPECT_TRUE(off == test.off && out == test.out && ok == test.ok)
            << "tzsetOffset(" << test.in << ") = " << off << ", " << out << ", " << ok;
    }
}

TEST_F(GoTimeZoneinfo, TzsetRule) {
    using hooks::Rule;
    struct {
        std::string in;
        Rule r;
        std::string out;
        bool ok;
    } tests[] = {
        {"", Rule{}, "", false},
        {"X", Rule{}, "", false},
        {"J10", Rule{.Kind = hooks::RuleJulian, .Day = 10, .Time = 2 * 60 * 60}, "", true},
        {"20", Rule{.Kind = hooks::RuleDOY, .Day = 20, .Time = 2 * 60 * 60}, "", true},
        {"M1.2.3", Rule{.Kind = hooks::RuleMonthWeekDay, .Day = 3, .Week = 2, .Mon = 1, .Time = 2 * 60 * 60}, "", true},
        {"30/03:00:00", Rule{.Kind = hooks::RuleDOY, .Day = 30, .Time = 3 * 60 * 60}, "", true},
        {"M4.5.6/03:00:00", Rule{.Kind = hooks::RuleMonthWeekDay, .Day = 6, .Week = 5, .Mon = 4, .Time = 3 * 60 * 60},
         "", true},
        {"M4.5.7/03:00:00", Rule{}, "", false},
        {"M4.5.6/-04", Rule{.Kind = hooks::RuleMonthWeekDay, .Day = 6, .Week = 5, .Mon = 4, .Time = -4 * 60 * 60}, "",
         true},
    };
    for (const auto& test : tests) {
        auto [r, out, ok] = hooks::TzsetRule(test.in);
        EXPECT_TRUE(r == test.r && out == test.out && ok == test.ok) << "tzsetRule(" << test.in << ")";
    }
}

TEST_F(GoTimeZoneinfo, EnvTZUsage) {
    scopedEnv restore("TZ", std::getenv("TZ"));
    struct reset_after {
        ~reset_after() { hooks::ForceUSPacificForTesting(); }
    } reset;

    std::string localZoneName = exists("/etc/localtime") ? "Local" : "UTC";
    struct {
        bool nilFlag;
        const char* tz;
        std::string local;
    } cases[] = {
        // no $TZ means use the system default /etc/localtime.
        {true, "", localZoneName},
        // $TZ="" means use UTC.
        {false, "", "UTC"},
        {false, ":", "UTC"},
        {false, "Asia/Shanghai", "Asia/Shanghai"},
        {false, ":Asia/Shanghai", "Asia/Shanghai"},
        {false, "/etc/localtime", localZoneName},
        {false, ":/etc/localtime", localZoneName},
    };
    for (const auto& c : cases) {
        hooks::ResetLocalOnceForTest();
        if (c.nilFlag) ::unsetenv("TZ");
        else ::setenv("TZ", c.tz, 1);
        EXPECT_EQ(S(time::Local->String()), c.local) << "invalid Local location name for " << c.tz;
    }

    hooks::ResetLocalOnceForTest();
    std::string path = "/usr/share/zoneinfo/Asia/Shanghai";
    ::setenv("TZ", path.c_str(), 1);
    if (!exists(path.c_str())) {
        EXPECT_EQ(S(time::Local->String()), "UTC") << "invalid path should fallback to UTC";
        return;
    }
    EXPECT_EQ(S(time::Local->String()), path) << "custom path should lead to path itself";

    time::Time timeInUTC = time::Date(2009, time::Month(1), 1, 12, 0, 0, 0, time::UTC);
    time::Time sameTimeInShanghai = time::Date(2009, time::Month(1), 1, 20, 0, 0, 0, time::Local);
    EXPECT_TRUE(timeInUTC.Equal(sameTimeInShanghai))
        << "invalid timezone: got " << S(timeInUTC.String()) << " want " << S(sameTimeInShanghai.String());

    hooks::ResetLocalOnceForTest();
    ::setenv("TZ", (":" + path).c_str(), 1);
    EXPECT_EQ(S(time::Local->String()), path) << "custom path should lead to path itself";

    hooks::ResetLocalOnceForTest();
    ::setenv("TZ", path.substr(0, path.size() - 1).c_str(), 1);
    EXPECT_EQ(S(time::Local->String()), "UTC") << "invalid path should fallback to UTC";
}
