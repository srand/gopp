#include <gopp/flag.hpp>
#include <gopp/io.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace gopp;

TEST(Time, ParseDuration) {
    struct {
        const char* in;
        int64 ns;
    } ok[] = {
        {"0", 0},
        {"5s", 5'000'000'000},
        {"30s", 30'000'000'000},
        {"1478s", 1478'000'000'000},
        {"-5s", -5'000'000'000},
        {"+5s", 5'000'000'000},
        {"1.5h", 5400'000'000'000},
        {"2h45m", 9900'000'000'000},
        {"300ms", 300'000'000},
        {"1.004s", 1'004'000'000},
        {"100us", 100'000},
        {"100\xc2\xb5s", 100'000},
        {"12ns", 12},
        {".5s", 500'000'000},
        {"1m1.5s", 61'500'000'000},
    };
    for (auto& c : ok) {
        auto [d, err] = time::ParseDuration(c.in);
        EXPECT_FALSE(err) << c.in << ": " << err;
        EXPECT_EQ(d.ns, c.ns) << c.in;
    }
    for (const char* bad : {"", "3", "-", "s", ".", ".s", "1.2.3s", "3000000h", "5parsecs"}) {
        auto [d, err] = time::ParseDuration(bad);
        EXPECT_TRUE(err) << bad;
    }
    // Round trip with Duration::to_string.
    auto d = 2 * time::Hour + 3 * time::Minute + 4500 * time::Millisecond;
    EXPECT_EQ(time::ParseDuration(d.String()).first, d);
}

TEST(Flag, Parse) {
    flag::FlagSet fs("test");
    string addr;
    isize n = 0;
    bool verbose = false;
    time::Duration d;
    fs.StringVar(&addr, "addr", "localhost:80", "address");
    fs.IntVar(&n, "n", 3, "count");
    fs.BoolVar(&verbose, "v", false, "verbose");
    fs.DurationVar(&d, "d", time::Second, "duration");
    EXPECT_EQ(addr, "localhost:80");
    EXPECT_EQ(n, 3);
    EXPECT_EQ(d, time::Second);

    std::vector<string> args = {"-addr", ":8080", "--n=7", "-v", "-d", "250ms", "rest", "-x"};
    EXPECT_FALSE(fs.Parse(args));
    EXPECT_EQ(addr, ":8080");
    EXPECT_EQ(n, 7);
    EXPECT_TRUE(verbose);
    EXPECT_EQ(d, 250 * time::Millisecond);
    ASSERT_EQ(fs.Args().size(), 2u);
    EXPECT_EQ(fs.Args()[0], "rest");
    EXPECT_EQ(fs.Args()[1], "-x");  // flags end at the first argument
}

TEST(Flag, Errors) {
    flag::FlagSet fs("test");
    isize n = 0;
    fs.IntVar(&n, "n", 0, "count");
    std::vector<string> unknown = {"-nope"};
    EXPECT_TRUE(fs.Parse(unknown));
    std::vector<string> missing = {"-n"};
    EXPECT_TRUE(fs.Parse(missing));
    std::vector<string> bad = {"-n", "x"};
    EXPECT_TRUE(fs.Parse(bad));
    std::vector<string> help = {"-h"};
    EXPECT_EQ(fs.Parse(help), flag::ErrHelp);
    std::vector<string> dashdash = {"--", "-n"};
    EXPECT_FALSE(fs.Parse(dashdash));
    EXPECT_EQ(fs.Args().size(), 1u);
}

TEST(Io, CopyN) {
    io::StringReader r("hello world");
    strings::Builder b;
    auto [n, err] = io::CopyN(b, r, 5);
    EXPECT_FALSE(err);
    EXPECT_EQ(n, 5);
    EXPECT_EQ(b.view(), "hello");
    auto [m, err2] = io::CopyN(b, r, 100);
    EXPECT_EQ(m, 6);
    EXPECT_EQ(err2, io::EOF_);
}
