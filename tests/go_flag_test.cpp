// Ported from Go 1.27.3's flag/flag_test.go, with Go's test names.
//
// Adapted: the tests that expect panics are death tests, as go++'s panic
// ends the program; TestExitCode runs its cases with EXPECT_EXIT instead of
// a child process; the zeroPanicker's type name is C++'s; TestChangingArgs
// and TestUsageOutput set os::Args with SetArgs. Left out:
// TestIntFlagOverflow (for 32-bit ints).
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gopp;

namespace {

std::string str(const string& s) { return std::string(s.view()); }

ptr<strings::Builder> newBuffer() { return make_ptr<strings::Builder>(); }

ptr<io::Writer> discard() {
    struct writer final : io::Writer {
        std::pair<isize, error> Write(std::span<const byte> p) override { return {static_cast<isize>(p.size()), nullptr}; }
    };
    return make_ptr<writer>();
}

std::vector<string> args(std::initializer_list<std::string_view> a) {
    std::vector<string> out;
    for (auto s : a) out.emplace_back(s);
    return out;
}

// Sets os::Args() for a test, and restores it.
struct setArgs {
    explicit setArgs(std::vector<std::string> a) : old_(os::Args().begin(), os::Args().end()) { set(a); }
    ~setArgs() {
        std::vector<std::string> o;
        for (auto& s : old_) o.push_back(str(s));
        set(o);
    }
    static void set(std::vector<std::string>& a) {
        std::vector<char*> argv;
        for (auto& s : a) argv.push_back(s.data());
        argv.push_back(nullptr);
        os::SetArgs(static_cast<int>(a.size()), argv.data());
    }
    std::vector<string> old_;
};

std::string boolString(std::string_view s) { return s == "0" ? "false" : "true"; }

}  // namespace

TEST(GoFlag, Everything) {
    flag::detail::resetForTesting(nullptr);
    flag::Bool("test_bool", false, "bool value");
    flag::Int("test_int", 0, "int value");
    flag::Int64("test_int64", 0, "int64 value");
    flag::Uint("test_uint", 0, "uint value");
    flag::Uint64("test_uint64", 0, "uint64 value");
    flag::String("test_string", "0", "string value");
    flag::Float64("test_float64", 0, "float64 value");
    flag::Duration("test_duration", time::Duration(0), "time.Duration value");
    flag::Func("test_func", "func value", [](string) -> error { return nullptr; });
    flag::BoolFunc("test_boolfunc", "func", [](string) -> error { return nullptr; });

    std::map<std::string, flag::Flag*> m;
    std::string desired = "0";
    auto visitor = [&](flag::Flag* f) {
        if (f->Name.size() > 5 && f->Name.starts_with("test_")) {
            m[str(f->Name)] = f;
            std::string v = str(f->Value->String());
            bool ok = v == desired || (f->Name == "test_bool" && v == boolString(desired)) ||
                      (f->Name == "test_duration" && v == desired + "s") || (f->Name == "test_func" && v.empty()) ||
                      (f->Name == "test_boolfunc" && v.empty());
            EXPECT_TRUE(ok) << "Visit: bad value " << v << " for " << f->Name;
        }
    };
    flag::VisitAll(visitor);
    EXPECT_EQ(m.size(), 10u) << "VisitAll misses some flags";
    m.clear();
    flag::Visit(visitor);
    EXPECT_EQ(m.size(), 0u) << "Visit sees unset flags";
    // Now set all flags.
    flag::Set("test_bool", "true");
    flag::Set("test_int", "1");
    flag::Set("test_int64", "1");
    flag::Set("test_uint", "1");
    flag::Set("test_uint64", "1");
    flag::Set("test_string", "1");
    flag::Set("test_float64", "1");
    flag::Set("test_duration", "1s");
    flag::Set("test_func", "1");
    flag::Set("test_boolfunc", "");
    desired = "1";
    flag::Visit(visitor);
    EXPECT_EQ(m.size(), 10u) << "Visit fails after set";
    // Visited in sorted order.
    std::vector<std::string> flagNames;
    flag::Visit([&](flag::Flag* f) { flagNames.push_back(str(f->Name)); });
    EXPECT_TRUE(std::is_sorted(flagNames.begin(), flagNames.end())) << "flag names not sorted";
}

TEST(GoFlag, Get) {
    flag::detail::resetForTesting(nullptr);
    flag::Bool("test_bool", true, "bool value");
    flag::Int("test_int", 1, "int value");
    flag::Int64("test_int64", 2, "int64 value");
    flag::Uint("test_uint", 3, "uint value");
    flag::Uint64("test_uint64", 4, "uint64 value");
    flag::String("test_string", "5", "string value");
    flag::Float64("test_float64", 6, "float64 value");
    flag::Duration("test_duration", time::Duration(7), "time.Duration value");

    flag::VisitAll([](flag::Flag* f) {
        if (f->Name.size() <= 5 || !f->Name.starts_with("test_")) return;
        auto* g = dynamic_cast<flag::Getter*>(f->Value.get());
        ASSERT_TRUE(g) << "Visit: value does not satisfy Getter: " << f->Name;
        std::any v = g->Get();
        bool ok = false;
        if (f->Name == "test_bool") ok = std::any_cast<bool>(v) == true;
        else if (f->Name == "test_int") ok = std::any_cast<isize>(v) == 1;
        else if (f->Name == "test_int64") ok = std::any_cast<int64>(v) == 2;
        else if (f->Name == "test_uint") ok = std::any_cast<std::size_t>(v) == 3;
        else if (f->Name == "test_uint64") ok = std::any_cast<uint64>(v) == 4;
        else if (f->Name == "test_string") ok = std::any_cast<string>(v) == "5";
        else if (f->Name == "test_float64") ok = std::any_cast<double>(v) == 6;
        else if (f->Name == "test_duration") ok = std::any_cast<time::Duration>(v) == time::Duration(7);
        EXPECT_TRUE(ok) << "Visit: bad value for " << f->Name;
    });
}

TEST(GoFlag, Usage) {
    bool called = false;
    flag::detail::resetForTesting([&] { called = true; });
    EXPECT_TRUE(flag::CommandLine().Parse(args({"-x"}))) << "parse did not fail for unknown flag";
    EXPECT_TRUE(called) << "did not call Usage for unknown flag";
    flag::detail::resetForTesting(nullptr);
}

namespace {

void testParse(flag::FlagSet& f) {
    EXPECT_FALSE(f.Parsed()) << "f.Parse() = true before Parse";
    bool* boolFlag = f.Bool("bool", false, "bool value");
    bool* bool2Flag = f.Bool("bool2", false, "bool2 value");
    isize* intFlag = f.Int("int", 0, "int value");
    int64* int64Flag = f.Int64("int64", 0, "int64 value");
    std::size_t* uintFlag = f.Uint("uint", 0, "uint value");
    uint64* uint64Flag = f.Uint64("uint64", 0, "uint64 value");
    string* stringFlag = f.String("string", "0", "string value");
    double* float64Flag = f.Float64("float64", 0, "float64 value");
    time::Duration* durationFlag = f.Duration("duration", 5 * time::Second, "time.Duration value");
    std::string extra = "one-extra-argument";
    auto a = args({"-bool", "-bool2=true", "--int", "22", "--int64", "0x23", "-uint", "24", "--uint64", "25", "-string", "hello",
                   "-float64", "2718e28", "-duration", "2m"});
    a.emplace_back(extra);
    ASSERT_FALSE(f.Parse(a));
    EXPECT_TRUE(f.Parsed()) << "f.Parse() = false after Parse";
    EXPECT_TRUE(*boolFlag);
    EXPECT_TRUE(*bool2Flag);
    EXPECT_EQ(*intFlag, 22);
    EXPECT_EQ(*int64Flag, 0x23);
    EXPECT_EQ(*uintFlag, 24u);
    EXPECT_EQ(*uint64Flag, 25u);
    EXPECT_EQ(*stringFlag, "hello");
    EXPECT_EQ(*float64Flag, 2718e28);
    EXPECT_EQ(*durationFlag, 2 * time::Minute);
    ASSERT_EQ(f.Args().size(), 1u) << "expected one argument";
    EXPECT_EQ(f.Args()[0], extra);
}

// flagVar: a user-defined flag type.
struct flagVar final : flag::Value {
    std::vector<std::string> v;
    string String() const override {
        std::string s = "[";
        for (std::size_t i = 0; i < v.size(); i++) s += (i ? " " : "") + v[i];
        return string(s + "]");
    }
    error Set(string value) override {
        v.push_back(str(value));
        return nullptr;
    }
};

}  // namespace

TEST(GoFlag, Parse) {
    flag::detail::resetForTesting([] { ADD_FAILURE() << "bad parse"; });
    testParse(flag::CommandLine());
    flag::detail::resetForTesting(nullptr);
}

TEST(GoFlag, FlagSetParse) { testParse(*flag::NewFlagSet("test", flag::ContinueOnError)); }

TEST(GoFlag, UserDefined) {
    flag::FlagSet flags("");
    flags.Init("test", flag::ContinueOnError);
    flags.SetOutput(discard());
    auto v = make_ptr<flagVar>();
    flags.Var(v, "v", "usage");
    EXPECT_FALSE(flags.Parse(args({"-v", "1", "-v", "2", "-v=3"})));
    ASSERT_EQ(v->v.size(), 3u);
    EXPECT_EQ(v->String(), "[1 2 3]");
}

TEST(GoFlag, UserDefinedFunc) {
    auto flags = flag::NewFlagSet("test", flag::ContinueOnError);
    flags->SetOutput(discard());
    std::vector<std::string> ss;
    flags->Func("v", "usage", [&](string s) -> error {
        ss.push_back(str(s));
        return nullptr;
    });
    EXPECT_FALSE(flags->Parse(args({"-v", "1", "-v", "2", "-v=3"})));
    ASSERT_EQ(ss.size(), 3u);
    EXPECT_EQ(ss, (std::vector<std::string>{"1", "2", "3"}));
    // The usage.
    auto buf = newBuffer();
    flags->SetOutput(buf);
    flags->Parse(args({"-h"}));
    EXPECT_NE(buf->String().find("usage"), string::npos) << "usage string not included: " << buf->String();
    // A Func error.
    flags = flag::NewFlagSet("test", flag::ContinueOnError);
    flags->SetOutput(discard());
    flags->Func("v", "usage", [](string) -> error { return fmt::Errorf("test error"); });
    // Not set: no error.
    EXPECT_FALSE(flags->Parse({}));
    // Set: the error.
    error err = flags->Parse(args({"-v", "1"}));
    ASSERT_TRUE(err) << "expected error; got none";
    EXPECT_NE(err.Error().find("test error"), string::npos) << err;
}

TEST(GoFlag, UserDefinedForCommandLine) {
    const std::string help = "HELP";
    std::string result;
    flag::detail::resetForTesting([&] { result = help; });
    flag::Usage();
    EXPECT_EQ(result, help);
    flag::detail::resetForTesting(nullptr);
}

namespace {

// boolFlagVar: a user-defined boolean flag type.
struct boolFlagVar final : flag::BoolFlag {
    int count = 0;
    string String() const override { return strconv::Itoa(count); }
    error Set(string value) override {
        if (value == "true") count++;
        return nullptr;
    }
    bool IsBoolFlag() const override { return count < 4; }
};

}  // namespace

TEST(GoFlag, UserDefinedBool) {
    flag::FlagSet flags("");
    flags.Init("test", flag::ContinueOnError);
    flags.SetOutput(discard());
    auto b = make_ptr<boolFlagVar>();
    flags.Var(b, "b", "usage");
    error err = flags.Parse(args({"-b", "-b", "-b", "-b=true", "-b=false", "-b", "barg", "-b"}));
    if (err) {
        EXPECT_GE(b->count, 4) << err;
    }
    EXPECT_EQ(b->count, 4);
    EXPECT_TRUE(err) << "expected error; got none";
}

TEST(GoFlag, UserDefinedBoolUsage) {
    flag::FlagSet flags("");
    flags.Init("test", flag::ContinueOnError);
    auto buf = newBuffer();
    flags.SetOutput(buf);
    auto b = make_ptr<boolFlagVar>();
    flags.Var(b, "b", "X");
    b->count = 0;
    // IsBoolFlag is true: the usage looks boolean.
    flags.PrintDefaults();
    EXPECT_EQ(buf->String(), "  -b\tX\n");
    b->count = 4;
    // IsBoolFlag is false: it doesn't.
    flags.PrintDefaults();
    EXPECT_EQ(buf->String(), "  -b\tX\n  -b value\n    \tX\n");
}

TEST(GoFlag, SetOutput) {
    flag::FlagSet flags("");
    auto buf = newBuffer();
    flags.SetOutput(buf);
    flags.Init("test", flag::ContinueOnError);
    flags.Parse(args({"-unknown"}));
    EXPECT_NE(buf->String().find("-unknown"), string::npos) << "expected output mentioning unknown; got " << buf->String();
}

// The flags can be reset: still works, but not well; FlagSet supersedes it.
TEST(GoFlag, ChangingArgs) {
    flag::detail::resetForTesting([] { ADD_FAILURE() << "bad parse"; });
    setArgs restore({"cmd", "-before", "subcmd", "-after", "args"});
    bool* before = flag::Bool("before", false, "");
    ASSERT_FALSE(flag::CommandLine().Parse(std::span<const string>(os::Args()).subspan(1)));
    string cmd = flag::Arg(0);
    std::vector<std::string> rest;
    for (auto& a : flag::Args()) rest.push_back(str(a));
    setArgs::set(rest);
    bool* after = flag::Bool("after", false, "");
    flag::Parse();
    auto a = flag::Args();
    EXPECT_TRUE(*before && cmd == "subcmd" && *after && a.size() == 1 && a[0] == "args")
        << "expected true subcmd true [args]";
    flag::detail::resetForTesting(nullptr);
}

// -help calls the usage function and returns ErrHelp.
TEST(GoFlag, Help) {
    bool helpCalled = false;
    auto fs = flag::NewFlagSet("help test", flag::ContinueOnError);
    fs->Usage = [&] { helpCalled = true; };
    bool f = false;
    fs->BoolVar(&f, "flag", false, "regular flag");
    // A regular flag.
    ASSERT_FALSE(fs->Parse(args({"-flag=true"})));
    EXPECT_TRUE(f) << "flag was not set by -flag";
    EXPECT_FALSE(helpCalled) << "help called for regular flag";
    helpCalled = false;
    // The help flag.
    error err = fs->Parse(args({"-help"}));
    ASSERT_TRUE(err) << "error expected";
    ASSERT_EQ(err, flag::ErrHelp);
    ASSERT_TRUE(helpCalled) << "help was not called";
    // A help flag of our own overrides it.
    bool help = false;
    fs->BoolVar(&help, "help", false, "help flag");
    helpCalled = false;
    ASSERT_FALSE(fs->Parse(args({"-help"})));
    ASSERT_FALSE(helpCalled) << "help was called; should not have been for defined help flag";
}

namespace {

// zeroPanicker: a Value whose String throws, as Go's panics, when its
// dontPanic field is false.
struct zeroPanicker final : flag::Value {
    zeroPanicker() = default;
    zeroPanicker(bool dp, std::string val) : dontPanic(dp), v(std::move(val)) {}
    bool dontPanic = false;
    std::string v;
    error Set(string s) override {
        v = str(s);
        return nullptr;
    }
    string String() const override {
        if (!dontPanic) throw std::runtime_error("panic!");
        return string(v);
    }
};

}  // namespace

TEST(GoFlag, PrintDefaults) {
    const std::string defaultOutput =
        "  -A\tfor bootstrapping, allow 'any' type\n"
        "  -Alongflagname\n"
        "    \tdisable bounds checking\n"
        "  -C\ta boolean defaulting to true (default true)\n"
        "  -D path\n"
        "    \tset relative path for local imports\n"
        "  -E string\n"
        "    \tissue 23543 (default \"0\")\n"
        "  -F number\n"
        "    \ta non-zero number (default 2.7)\n"
        "  -G float\n"
        "    \ta float that defaults to zero\n"
        "  -M string\n"
        "    \ta multiline\n"
        "    \thelp\n"
        "    \tstring\n"
        "  -N int\n"
        "    \ta non-zero int (default 27)\n"
        "  -O\ta flag\n"
        "    \tmultiline help string (default true)\n"
        "  -V list\n"
        "    \ta list of strings (default [a b])\n"
        "  -Z int\n"
        "    \tan int that defaults to zero\n"
        "  -ZP0 value\n"
        "    \ta flag whose String method panics when it is zero\n"
        "  -ZP1 value\n"
        "    \ta flag whose String method panics when it is zero\n"
        "  -maxT timeout\n"
        "    \tset timeout for dial\n"
        "\n"
        "panic calling String method on zero (anonymous namespace)::zeroPanicker for flag ZP0: panic!\n"
        "panic calling String method on zero (anonymous namespace)::zeroPanicker for flag ZP1: panic!\n";

    auto fs = flag::NewFlagSet("print defaults test", flag::ContinueOnError);
    auto buf = newBuffer();
    fs->SetOutput(buf);
    fs->Bool("A", false, "for bootstrapping, allow 'any' type");
    fs->Bool("Alongflagname", false, "disable bounds checking");
    fs->Bool("C", true, "a boolean defaulting to true");
    fs->String("D", "", "set relative `path` for local imports");
    fs->String("E", "0", "issue 23543");
    fs->Float64("F", 2.7, "a non-zero `number`");
    fs->Float64("G", 0, "a float that defaults to zero");
    fs->String("M", "", "a multiline\nhelp\nstring");
    fs->Int("N", 27, "a non-zero int");
    fs->Bool("O", true, "a flag\nmultiline help string");
    auto list = make_ptr<flagVar>();
    list->v = {"a", "b"};
    fs->Var(list, "V", "a `list` of strings");
    fs->Int("Z", 0, "an int that defaults to zero");
    fs->Var(make_ptr<zeroPanicker>(true, ""), "ZP0", "a flag whose String method panics when it is zero");
    fs->Var(make_ptr<zeroPanicker>(true, "something"), "ZP1", "a flag whose String method panics when it is zero");
    fs->Duration("maxT", time::Duration(0), "set `timeout` for dial");
    fs->PrintDefaults();
    EXPECT_EQ(str(buf->String()), defaultOutput);
}

// Issue 20998: Usage respects CommandLine's output.
TEST(GoFlag, UsageOutput) {
    flag::detail::resetForTesting(flag::detail::defaultUsage);
    auto buf = newBuffer();
    flag::CommandLine().SetOutput(buf);
    setArgs restore({"app", "-i=1", "-unknown"});
    flag::Parse();
    EXPECT_EQ(str(buf->String()), "flag provided but not defined: -i\nUsage of app:\n");
    flag::detail::resetForTesting(flag::detail::defaultUsage);
}

TEST(GoFlag, Getters) {
    auto fs = flag::NewFlagSet("flag set", flag::ContinueOnError);
    EXPECT_EQ(fs->Name(), "flag set");
    EXPECT_EQ(fs->ErrorHandling(), flag::ContinueOnError);
    EXPECT_EQ(&fs->Output(), static_cast<io::Writer*>(&os::Stderr));

    auto out = newBuffer();
    fs->Init("gopher", flag::ExitOnError);
    fs->SetOutput(out);
    EXPECT_EQ(fs->Name(), "gopher");
    EXPECT_EQ(fs->ErrorHandling(), flag::ExitOnError);
    EXPECT_EQ(&fs->Output(), static_cast<io::Writer*>(out.get()));
}

TEST(GoFlag, ParseError) {
    for (std::string typ : {"bool", "int", "int64", "uint", "uint64", "float64", "duration"}) {
        auto fs = flag::NewFlagSet("parse error test", flag::ContinueOnError);
        fs->SetOutput(discard());
        fs->Bool("bool", false, "");
        fs->Int("int", 0, "");
        fs->Int64("int64", 0, "");
        fs->Uint("uint", 0, "");
        fs->Uint64("uint64", 0, "");
        fs->Float64("float64", 0, "");
        fs->Duration("duration", time::Duration(0), "");
        // Strings can't fail; x isn't valid for the others.
        std::vector<string> a = {string("-" + typ + "=x")};
        error err = fs->Parse(a);
        ASSERT_TRUE(err) << "Parse(" << a[0] << ") = nil; expected parse error";
        EXPECT_TRUE(err.Error().find("invalid") != string::npos && err.Error().find("parse error") != string::npos) << err;
    }
}

TEST(GoFlag, RangeError) {
    for (std::string_view arg : {"-int=123456789012345678901", "-int64=123456789012345678901", "-uint=123456789012345678901",
                                 "-uint64=123456789012345678901", "-float64=1e1000"}) {
        auto fs = flag::NewFlagSet("parse error test", flag::ContinueOnError);
        fs->SetOutput(discard());
        fs->Int("int", 0, "");
        fs->Int64("int64", 0, "");
        fs->Uint("uint", 0, "");
        fs->Uint64("uint64", 0, "");
        fs->Float64("float64", 0, "");
        error err = fs->Parse(args({arg}));
        ASSERT_TRUE(err) << "Parse(" << arg << ") = nil; expected range error";
        EXPECT_TRUE(err.Error().find("invalid") != string::npos && err.Error().find("value out of range") != string::npos) << err;
    }
}

TEST(GoFlag, ExitCode) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    constexpr int magic = 123;
    struct test {
        std::string flag, flagHandle;
        int expectExit;
    };
    test tests[] = {
        {"-h", "", 0}, {"-help", "", 0}, {"-undefined", "", 2}, {"-h", "h", magic}, {"-help", "help", magic},
    };
    for (auto& t : tests) {
        EXPECT_EXIT(
            {
                auto fs = flag::NewFlagSet("test", flag::ExitOnError);
                fs->SetOutput(discard());
                bool b = false;
                if (!t.flagHandle.empty()) fs->BoolVar(&b, t.flagHandle, false, "");
                fs->Parse(args({t.flag}));
                os::Exit(magic);
            },
            testing::ExitedWithCode(t.expectExit), "")
            << "flag " << t.flag << " handle " << t.flagHandle;
    }
}

TEST(GoFlag, InvalidFlags) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::pair<std::string_view, std::string> tests[] = {
        {"-foo", "flag \"-foo\" begins with -"},
        {"foo=bar", "flag \"foo=bar\" contains ="},
    };
    for (auto& [name, errorMsg] : tests) {
        EXPECT_DEATH(
            {
                auto fs = flag::NewFlagSet("", flag::ContinueOnError);
                fs->SetOutput(discard());
                fs->Var(make_ptr<flagVar>(), name, "");
            },
            errorMsg);
        // The message goes to the output too.
        auto fs = flag::NewFlagSet("", flag::ContinueOnError);
        auto buf = newBuffer();
        fs->SetOutput(buf);
        EXPECT_DEATH(fs->Var(make_ptr<flagVar>(), name, ""), "");
    }
}

TEST(GoFlag, RedefinedFlags) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::pair<std::string, std::string> tests[] = {
        {"", "flag redefined: foo"},
        {"fs", "fs flag redefined: foo"},
    };
    for (auto& [flagSetName, errorMsg] : tests) {
        auto fs = flag::NewFlagSet(string(flagSetName), flag::ContinueOnError);
        auto buf = newBuffer();
        fs->SetOutput(buf);
        auto v = make_ptr<flagVar>();
        fs->Var(v, "foo", "");
        EXPECT_DEATH(fs->Var(v, "foo", ""), errorMsg);
    }
}

TEST(GoFlag, UserDefinedBoolFunc) {
    auto flags = flag::NewFlagSet("test", flag::ContinueOnError);
    flags->SetOutput(discard());
    std::vector<std::string> ss;
    flags->BoolFunc("v", "usage", [&](string s) -> error {
        ss.push_back(str(s));
        return nullptr;
    });
    EXPECT_FALSE(flags->Parse(args({"-v", "", "-v", "1", "-v=2"})));
    ASSERT_EQ(ss.size(), 1u) << "want 1 arg";
    EXPECT_EQ(ss[0], "true");
    // The usage.
    auto buf = newBuffer();
    flags->SetOutput(buf);
    flags->Parse(args({"-h"}));
    EXPECT_NE(buf->String().find("usage"), string::npos) << "usage string not included: " << buf->String();
    // A BoolFunc error.
    flags = flag::NewFlagSet("test", flag::ContinueOnError);
    flags->SetOutput(discard());
    flags->BoolFunc("v", "usage", [](string) -> error { return fmt::Errorf("test error"); });
    // Not set: no error.
    EXPECT_FALSE(flags->Parse({}));
    // Set: the error.
    error err = flags->Parse(args({"-v", ""}));
    ASSERT_TRUE(err) << "got err == nil; want err != nil";
    EXPECT_NE(err.Error().find("test error"), string::npos) << err;
}

TEST(GoFlag, DefineAfterSet) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto flags = flag::NewFlagSet("test", flag::ContinueOnError);
    // Set alone doesn't panic.
    flags->Set("myFlag", "value");
    // Defining after it does.
    EXPECT_DEATH(flags->String("myFlag", "default", "usage"), "flag myFlag set at .*go_flag_test.cpp:.* before being defined");
}
