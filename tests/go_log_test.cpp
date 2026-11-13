// Ported from Go 1.27.3's log/log_test.go, with Go's test names. The
// formats are std::format's: Println("hello", 23, "world") and
// Printf("hello %d world", 23) are Println and Print of "hello {} world".
// TestDiscard counts formatting calls, not allocations; TestCallDepth
// runs the calls in death tests, where Go runs the test program again;
// and the benchmarks are left out.
#include <gopp/io.hpp>
#include <gopp/log.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <format>
#include <regex>
#include <string>

using namespace gopp;

namespace {

const std::string Rdate = "[0-9][0-9][0-9][0-9]/[0-9][0-9]/[0-9][0-9]";
const std::string Rtime = "[0-9][0-9]:[0-9][0-9]:[0-9][0-9]";
const std::string Rmicroseconds = "\\.[0-9][0-9][0-9][0-9][0-9][0-9]";

// The lines of the calls to log::Print and log::Println in testPrint.
int print_line, println_line;

std::string Rline() { return "(" + std::to_string(print_line) + "|" + std::to_string(println_line) + "):"; }
std::string Rlongfile() { return ".*/[A-Za-z0-9_\\-]+\\.cpp:" + Rline(); }
std::string Rshortfile() { return "[A-Za-z0-9_\\-]+\\.cpp:" + Rline(); }

struct tester {
    int flag;
    std::string prefix;
    std::function<std::string()> pattern;  // regexp that log output must match; we add ^ and expected_text$ always
};

std::vector<tester> tests() {
    using namespace log;
    return {
        // individual pieces:
        {0, "", [] { return ""; }},
        {0, "XXX", [] { return "XXX"; }},
        {Ldate, "", [] { return Rdate + " "; }},
        {Ltime, "", [] { return Rtime + " "; }},
        {Ltime | Lmsgprefix, "XXX", [] { return Rtime + " XXX"; }},
        {Ltime | Lmicroseconds, "", [] { return Rtime + Rmicroseconds + " "; }},
        {Lmicroseconds, "", [] { return Rtime + Rmicroseconds + " "; }},  // microsec implies time
        {Llongfile, "", [] { return Rlongfile() + " "; }},
        {Lshortfile, "", [] { return Rshortfile() + " "; }},
        {Llongfile | Lshortfile, "", [] { return Rshortfile() + " "; }},  // shortfile overrides longfile
        // everything at once:
        {Ldate | Ltime | Lmicroseconds | Llongfile, "XXX",
         [] { return "XXX" + Rdate + " " + Rtime + Rmicroseconds + " " + Rlongfile() + " "; }},
        {Ldate | Ltime | Lmicroseconds | Lshortfile, "XXX",
         [] { return "XXX" + Rdate + " " + Rtime + Rmicroseconds + " " + Rshortfile() + " "; }},
        {Ldate | Ltime | Lmicroseconds | Llongfile | Lmsgprefix, "XXX",
         [] { return Rdate + " " + Rtime + Rmicroseconds + " " + Rlongfile() + " XXX"; }},
        {Ldate | Ltime | Lmicroseconds | Lshortfile | Lmsgprefix, "XXX",
         [] { return Rdate + " " + Rtime + Rmicroseconds + " " + Rshortfile() + " XXX"; }},
    };
}

// Test using Println("hello {} world", 23) or Print("hello {} world", 23).
void testPrint(int flag, const std::string& prefix, std::string pattern, bool useFormat) {
    auto buf = make_ptr<strings::Builder>();
    log::SetOutput(buf);
    log::SetFlags(flag);
    log::SetPrefix(prefix);
    if (useFormat) {
        log::Print("hello {} world", 23); print_line = __LINE__;
    } else {
        log::Println("hello {} world", 23); println_line = __LINE__;
    }
    std::string line(buf->view());
    line = line.substr(0, line.size() - 1);
    pattern = "^" + pattern + "hello 23 world$";
    EXPECT_TRUE(std::regex_search(line, std::regex(pattern))) << "log output should match " << pattern << " is " << line;
    log::SetOutput(ptr<io::Writer>(&os::Stderr));
}

// A value that counts how often it's formatted.
int formatted = 0;
struct counted {};

}  // namespace

template <>
struct std::formatter<counted> : std::formatter<std::string_view> {
    auto format(const counted&, auto& ctx) const {
        formatted++;
        return std::formatter<std::string_view>::format("counted", ctx);
    }
};

namespace {

TEST(GoLog, Default) {
    auto got = log::Default();
    EXPECT_EQ(got, log::Default()) << "Default should be std";
}

TEST(GoLog, All) {
    // Run once, to learn the lines of the calls.
    testPrint(0, "", "", false);
    testPrint(0, "", "", true);
    for (const auto& testcase : tests()) {
        testPrint(testcase.flag, testcase.prefix, testcase.pattern(), false);
        testPrint(testcase.flag, testcase.prefix, testcase.pattern(), true);
    }
    log::SetFlags(log::LstdFlags);
    log::SetPrefix("");
}

TEST(GoLog, Output) {
    const std::string testString = "test";
    auto b = make_ptr<strings::Builder>();
    auto l = log::New(b, "", 0);
    l->Println("{}", testString);
    EXPECT_EQ(b->view(), testString + "\n");
}

TEST(GoLog, NonNewLogger) {
    auto l = make_ptr<log::Logger>();
    l->SetOutput(make_ptr<strings::Builder>());  // minimal work to initialize a Logger
    l->Print("hello");
}

TEST(GoLog, OutputRace) {
    auto b = make_ptr<strings::Builder>();
    auto l = log::New(b, "", 0);
    sync::WaitGroup wg;
    wg.Add(100);
    for (int i = 0; i < 100; i++) {
        go([&] {
            l->SetFlags(0);
            l->Output(0, "");
            wg.Done();
        });
    }
    wg.Wait();
    EXPECT_EQ(b->view(), std::string(100, '\n'));
}

TEST(GoLog, FlagAndPrefixSetting) {
    auto b = make_ptr<strings::Builder>();
    auto l = log::New(b, "Test:", log::LstdFlags);
    int f = l->Flags();
    EXPECT_EQ(f, log::LstdFlags) << "Flags 1";
    l->SetFlags(f | log::Lmicroseconds);
    f = l->Flags();
    EXPECT_EQ(f, log::LstdFlags | log::Lmicroseconds) << "Flags 2";
    EXPECT_EQ(l->Prefix(), "Test:");
    l->SetPrefix("Reality:");
    EXPECT_EQ(l->Prefix(), "Reality:");
    // Verify a log message looks right, with our prefix and microseconds present.
    l->Print("hello");
    std::string pattern = "^Reality:" + Rdate + " " + Rtime + Rmicroseconds + " hello\n";
    EXPECT_TRUE(std::regex_search(std::string(b->view()), std::regex(pattern))) << "message did not match pattern";

    // Ensure that a newline is added only if the buffer lacks a newline suffix.
    b->reset();
    l->SetFlags(0);
    l->SetPrefix("\n");
    l->Output(0, "");
    EXPECT_EQ(b->view(), "\n");
}

TEST(GoLog, UTCFlag) {
    auto b = make_ptr<strings::Builder>();
    auto l = log::New(b, "Test:", log::LstdFlags);
    l->SetFlags(log::Ldate | log::Ltime | log::LUTC);
    // Verify a log message looks right in the right time zone. Quantize to the second only.
    auto now = time::Now().UTC();
    l->Print("hello");
    auto want = [](time::Time now) {
        return std::format("Test:{}/{:02}/{:02} {:02}:{:02}:{:02} hello\n", now.Year(), static_cast<int>(now.Month()),
                           now.Day(), now.Hour(), now.Minute(), now.Second());
    };
    std::string got(b->view());
    if (got == want(now)) return;
    // It's possible we crossed a second boundary between getting now and logging,
    // so add a second and try again. This should very nearly always work.
    now = now.Add(time::Second);
    EXPECT_EQ(got, want(now));
}

TEST(GoLog, EmptyPrintCreatesLine) {
    auto b = make_ptr<strings::Builder>();
    auto l = log::New(b, "Header:", log::LstdFlags);
    l->Print();
    l->Println("non-empty");
    std::string output(b->view());
    auto count = [&](std::string_view sub) {
        int n = 0;
        for (auto i = output.find(sub); i != output.npos; i = output.find(sub, i + sub.size())) n++;
        return n;
    };
    EXPECT_EQ(count("Header"), 2) << "expected 2 headers";
    EXPECT_EQ(count("\n"), 2) << "expected 2 lines";
}

// Go counts allocations; here, a Logger writing to io::Discard doesn't
// format the message at all.
TEST(GoLog, Discard) {
    auto l = log::New(ptr<io::Writer>(&io::Discard()), "", 0);
    formatted = 0;
    for (int i = 0; i < 100; i++) l->Print("{}", counted{});
    EXPECT_EQ(formatted, 0);
}

TEST(GoLogDeathTest, CallDepth) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Each call is on a line of its own, which the output must name.
    auto run = [](std::function<void()> f) {
        log::SetFlags(log::Lshortfile);
        f();
        os::Exit(1);
    };
    auto want = [](int line, const char* name) { return std::format("go_log_test\\.cpp:{}: {}\n", line, name); };
    int line = __LINE__;
    EXPECT_EXIT(run([] { log::Fatal("Fatal"); }), testing::ExitedWithCode(1), want(line + 1, "Fatal"));
    EXPECT_EXIT(run([] { log::Fatalln("Fatalln"); }), testing::ExitedWithCode(1), want(line + 2, "Fatalln"));
    EXPECT_EXIT(run([] { log::Output(1, "Output"); }), testing::ExitedWithCode(1), want(line + 3, "Output"));
    EXPECT_DEATH(run([] { log::Panic("Panic"); }), want(line + 4, "Panic"));
    EXPECT_DEATH(run([] { log::Panicln("Panicln"); }), want(line + 5, "Panicln"));
    EXPECT_EXIT(run([] { log::Default()->Fatal("Default.Fatal"); }), testing::ExitedWithCode(1), want(line + 6, "Default.Fatal"));
    EXPECT_EXIT(run([] { log::Default()->Fatalln("Default.Fatalln"); }), testing::ExitedWithCode(1), want(line + 7, "Default.Fatalln"));
    EXPECT_EXIT(run([] { log::Default()->Output(1, "Default.Output"); }), testing::ExitedWithCode(1), want(line + 8, "Default.Output"));
    EXPECT_DEATH(run([] { log::Default()->Panic("Default.Panic"); }), want(line + 9, "Default.Panic"));
    EXPECT_DEATH(run([] { log::Default()->Panicln("Default.Panicln"); }), want(line + 10, "Default.Panicln"));
}

}  // namespace
