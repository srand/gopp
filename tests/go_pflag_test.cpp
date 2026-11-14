// Ported from spf13/pflag v1.0.10's flag_test.go and the tests of its value
// types (string_slice_test.go, string_to_string_test.go, count_test.go,
// ...), with pflag's test names.
#include <gopp/bytes.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/net/netip.hpp>
#include <gopp/pflag.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace gopp;

namespace {

// Custom value that satisfies the Value interface.
struct customValue final : pflag::Value {
    int64 v = 0;
    explicit customValue(int64 x = 0) : v(x) {}
    string String() const override { return strconv::Itoa(v); }
    error Set(string s) override {
        auto [n, err] = strconv::ParseInt(s, 0, 64);
        v = n;
        return err;
    }
    string Type() const override { return "custom"_s; }
};

}  // namespace

TEST(GoPflag, FlagSetParse) {
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    EXPECT_FALSE(f->Parsed()) << "f.Parse() = true before Parse";
    bool* boolFlag = f->Bool("bool", false, "bool value");
    bool* bool2Flag = f->Bool("bool2", false, "bool2 value");
    bool* bool3Flag = f->Bool("bool3", false, "bool3 value");
    isize* intFlag = f->Int("int", 0, "int value");
    int8* int8Flag = f->Int8("int8", 0, "int value");
    int16* int16Flag = f->Int16("int16", 0, "int value");
    int32* int32Flag = f->Int32("int32", 0, "int value");
    int64* int64Flag = f->Int64("int64", 0, "int64 value");
    std::size_t* uintFlag = f->Uint("uint", 0, "uint value");
    uint8* uint8Flag = f->Uint8("uint8", 0, "uint value");
    uint16* uint16Flag = f->Uint16("uint16", 0, "uint value");
    uint32* uint32Flag = f->Uint32("uint32", 0, "uint value");
    uint64* uint64Flag = f->Uint64("uint64", 0, "uint64 value");
    string* stringFlag = f->String("string", "0", "string value");
    float* float32Flag = f->Float32("float32", 0, "float32 value");
    double* float64Flag = f->Float64("float64", 0, "float64 value");
    net::IP* ipFlag = f->IP("ip", net::ParseIP("127.0.0.1"), "ip value");
    net::IPMask* maskFlag = f->IPMask("mask", pflag::ParseIPv4Mask("0.0.0.0"), "mask value");
    time::Duration* durationFlag = f->Duration("duration", 5 * time::Second, "time.Duration value");
    isize* optionalIntNoValueFlag = f->Int("optional-int-no-value", 0, "int value");
    f->Lookup("optional-int-no-value")->NoOptDefVal = "9";
    isize* optionalIntWithValueFlag = f->Int("optional-int-with-value", 0, "int value");
    f->Lookup("optional-int-no-value")->NoOptDefVal = "9";
    string extra = "one-extra-argument";
    std::vector<string> args = {
        "--bool", "--bool2=true", "--bool3=false", "--int=22", "--int8=-8", "--int16=-16", "--int32=-32", "--int64=0x23",
        "--uint", "24", "--uint8=8", "--uint16=16", "--uint32=32", "--uint64=25", "--string=hello", "--float32=-172e12",
        "--float64=2718e28", "--ip=10.11.12.13", "--mask=255.255.255.0", "--duration=2m", "--optional-int-no-value",
        "--optional-int-with-value=42", extra,
    };
    ASSERT_FALSE(f->Parse(args));
    EXPECT_TRUE(f->Parsed());
    EXPECT_TRUE(*boolFlag);
    EXPECT_EQ(f->GetBool("bool"), std::pair(*boolFlag, error()));
    EXPECT_TRUE(*bool2Flag);
    EXPECT_FALSE(*bool3Flag);
    EXPECT_EQ(*intFlag, 22);
    EXPECT_EQ(f->GetInt("int").first, *intFlag);
    EXPECT_EQ(*int8Flag, -8);
    EXPECT_EQ(*int16Flag, -16);
    EXPECT_EQ(f->GetInt8("int8").first, *int8Flag);
    EXPECT_EQ(f->GetInt16("int16").first, *int16Flag);
    EXPECT_EQ(*int32Flag, -32);
    EXPECT_EQ(f->GetInt32("int32").first, *int32Flag);
    EXPECT_EQ(*int64Flag, 0x23);
    EXPECT_EQ(f->GetInt64("int64").first, *int64Flag);
    EXPECT_EQ(*uintFlag, 24u);
    EXPECT_EQ(f->GetUint("uint").first, *uintFlag);
    EXPECT_EQ(*uint8Flag, 8);
    EXPECT_EQ(f->GetUint8("uint8").first, *uint8Flag);
    EXPECT_EQ(*uint16Flag, 16);
    EXPECT_EQ(f->GetUint16("uint16").first, *uint16Flag);
    EXPECT_EQ(*uint32Flag, 32u);
    EXPECT_EQ(f->GetUint32("uint32").first, *uint32Flag);
    EXPECT_EQ(*uint64Flag, 25u);
    EXPECT_EQ(f->GetUint64("uint64").first, *uint64Flag);
    EXPECT_EQ(*stringFlag, "hello");
    EXPECT_EQ(f->GetString("string").first, *stringFlag);
    EXPECT_EQ(*float32Flag, -172e12f);
    EXPECT_EQ(f->GetFloat32("float32").first, *float32Flag);
    EXPECT_EQ(*float64Flag, 2718e28);
    EXPECT_EQ(f->GetFloat64("float64").first, *float64Flag);
    EXPECT_TRUE(ipFlag->Equal(net::ParseIP("10.11.12.13"))) << "ip flag should be 10.11.12.13, is " << ipFlag->String();
    EXPECT_TRUE(f->GetIP("ip").first.Equal(*ipFlag));
    EXPECT_EQ(maskFlag->String(), pflag::ParseIPv4Mask("255.255.255.0").String());
    auto [mask, merr] = f->GetIPv4Mask("mask");
    EXPECT_FALSE(merr) << merr;
    EXPECT_EQ(mask.String(), maskFlag->String());
    EXPECT_EQ(*durationFlag, 2 * time::Minute);
    EXPECT_EQ(f->GetDuration("duration").first, *durationFlag);
    EXPECT_TRUE(f->GetInt("duration").second) << "GetInt parsed a time.Duration?!?!";
    EXPECT_EQ(*optionalIntNoValueFlag, 9);
    EXPECT_EQ(*optionalIntWithValueFlag, 42);
    ASSERT_EQ(f->Args().size(), 1u);
    EXPECT_EQ(f->Args()[0], extra);

    // Test unknown.
    auto err = f->Parse(std::vector<string>{"--unknown"});
    ASSERT_TRUE(err) << "parse did not fail for unknown flag";
    EXPECT_EQ(err.Error(), "unknown flag: --unknown");
    // Test invalid.
    err = f->Parse(std::vector<string>{"--bool=abcdefg"});
    ASSERT_TRUE(err) << "parse did not fail for invalid argument";
    EXPECT_EQ(err.Error(), "invalid argument \"abcdefg\" for \"--bool\" flag: strconv.ParseBool: parsing \"abcdefg\": invalid syntax");
    // Test required.
    err = f->Parse(std::vector<string>{"--int"});
    ASSERT_TRUE(err) << "parse did not fail for missing argument";
    EXPECT_EQ(err.Error(), "flag needs an argument: --int");
}

TEST(GoPflag, Shorthand) {
    auto f = pflag::NewFlagSet("shorthand", pflag::ContinueOnError);
    bool* boolaFlag = f->BoolP("boola", "a", false, "bool value");
    bool* boolbFlag = f->BoolP("boolb", "b", false, "bool2 value");
    bool* boolcFlag = f->BoolP("boolc", "c", false, "bool3 value");
    bool* booldFlag = f->BoolP("boold", "d", false, "bool4 value");
    string* stringaFlag = f->StringP("stringa", "s", "0", "string value");
    string* stringzFlag = f->StringP("stringz", "z", "0", "string value");
    string extra = "interspersed-argument";
    string notaflag = "--i-look-like-a-flag";
    std::vector<string> args = {"-ab", extra, "-cs", "hello", "-z=something", "-d=true", "--", notaflag};
    f->SetOutput(make_ptr<bytes::Buffer>());
    ASSERT_FALSE(f->Parse(args));
    EXPECT_TRUE(*boolaFlag);
    EXPECT_TRUE(*boolbFlag);
    EXPECT_TRUE(*boolcFlag);
    EXPECT_TRUE(*booldFlag);
    EXPECT_EQ(*stringaFlag, "hello");
    EXPECT_EQ(*stringzFlag, "something");
    ASSERT_EQ(f->Args().size(), 2u);
    EXPECT_EQ(f->Args()[0], extra);
    EXPECT_EQ(f->Args()[1], notaflag);
    EXPECT_EQ(f->ArgsLenAtDash(), 1);

    // Test unknown.
    auto err = f->Parse(std::vector<string>{"-ukn"});
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "unknown shorthand flag: 'u' in -ukn");
    // Test required.
    err = f->Parse(std::vector<string>{"-as"});
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "flag needs an argument: 's' in -s");
}

TEST(GoPflag, ShorthandLookup) {
    auto f = pflag::NewFlagSet("shorthand", pflag::ContinueOnError);
    f->BoolP("boola", "a", false, "bool value");
    f->BoolP("boolb", "b", false, "bool2 value");
    std::vector<string> args = {"-ab"};
    f->SetOutput(make_ptr<bytes::Buffer>());
    ASSERT_FALSE(f->Parse(args));
    auto* flag = f->ShorthandLookup("a");
    ASSERT_TRUE(flag) << "f.ShorthandLookup(\"a\") returned nil";
    EXPECT_EQ(flag->Name, "boola");
    EXPECT_FALSE(f->ShorthandLookup(""));
    EXPECT_DEATH(f->ShorthandLookup("ab"), "can not look up shorthand which is more than one ASCII character");
}

TEST(GoPflag, IgnoreUnknownFlags) {
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->ParseErrorsAllowlist.UnknownFlags = true;
    f->BoolP("boola", "a", false, "bool value");
    f->BoolP("boolb", "b", false, "bool2 value");
    f->BoolP("boolc", "c", false, "bool3 value");
    f->BoolP("boold", "d", false, "bool4 value");
    f->BoolP("boole", "e", false, "bool4 value");
    f->StringP("stringa", "s", "0", "string value");
    f->StringP("stringz", "z", "0", "string value");
    f->StringP("stringx", "x", "0", "string value");
    f->StringP("stringy", "y", "0", "string value");
    f->StringP("stringo", "o", "0", "string value");
    f->Lookup("stringx")->NoOptDefVal = "1";
    std::vector<string> args = {
        "-ab", "-cs=xx", "--stringz=something", "--unknown1", "unknown1Value", "-d=true", "-x", "--unknown2=unknown2Value",
        "-u=unknown3Value", "-p", "unknown4Value",
        "-q",  // another unknown with bool value
        "-y", "ee", "--unknown7=unknown7value", "--stringo=ovalue", "--unknown8=unknown8value", "--boole", "--unknown6", "",
        "-uuuuu", "", "--unknown10", "--unknown11",
    };
    std::vector<std::string> want = {
        "boola", "true", "boolb", "true", "boolc", "true", "stringa", "xx", "stringz", "something",
        "boold", "true", "stringx", "1", "stringy", "ee", "stringo", "ovalue", "boole", "true",
    };
    std::vector<std::string> got;
    auto store = [&](pflag::Flag* flag, string value) -> error {
        got.push_back(flag->Name.str());
        if (!value.empty()) got.push_back(value.str());
        return nullptr;
    };
    ASSERT_FALSE(f->ParseAll(args, store));
    EXPECT_TRUE(f->Parsed());
    EXPECT_EQ(got, want) << "f.ParseAll() fail to restore the args";
}

TEST(GoPflag, AddFlagSet) {
    auto oldSet = pflag::NewFlagSet("old", pflag::ContinueOnError);
    auto newSet = pflag::NewFlagSet("new", pflag::ContinueOnError);
    oldSet->String("flag1", "flag1", "flag1");
    oldSet->String("flag2", "flag2", "flag2");
    newSet->String("flag2", "flag2", "flag2");
    newSet->String("flag3", "flag3", "flag3");
    oldSet->AddFlagSet(newSet.get());
    int n = 0;
    oldSet->VisitAll([&](pflag::Flag*) { n++; });
    EXPECT_EQ(n, 3) << "Unexpected result adding a FlagSet to a FlagSet";
}

TEST(GoPflag, Annotation) {
    auto f = pflag::NewFlagSet("shorthand", pflag::ContinueOnError);
    auto err = f->SetAnnotation("missing-flag", "key", {});
    ASSERT_TRUE(err) << "Expected error setting annotation on non-existent flag";
    EXPECT_EQ(err.Error(), "no such flag -missing-flag");

    f->StringP("stringa", "a", "", "string value");
    EXPECT_FALSE(f->SetAnnotation("stringa", "key", {}));
    EXPECT_TRUE(f->Lookup("stringa")->Annotations["key"_s].empty());

    f->StringP("stringb", "b", "", "string2 value");
    EXPECT_FALSE(f->SetAnnotation("stringb", "key", {"value1"_s}));
    EXPECT_EQ(f->Lookup("stringb")->Annotations["key"_s], std::vector<string>{"value1"});
    EXPECT_FALSE(f->SetAnnotation("stringb", "key", {"value2"_s}));
    EXPECT_EQ(f->Lookup("stringb")->Annotations["key"_s], std::vector<string>{"value2"});
}

TEST(GoPflag, Name) {
    std::string flagSetName = "bob";
    auto f = pflag::NewFlagSet(string(flagSetName), pflag::ContinueOnError);
    EXPECT_EQ(f->Name(), flagSetName);
}

TEST(GoPflag, ParseRepeated) {
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    string* s = f->String("s", "", "");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--s=a", "--s=b"}));
    EXPECT_EQ(*s, "b");
}

TEST(GoPflag, ChangedHelper) {
    auto f = pflag::NewFlagSet("changedtest", pflag::ContinueOnError);
    f->Bool("changed", false, "changed bool");
    f->Bool("settrue", true, "true to true");
    f->Bool("setfalse", false, "false to false");
    f->Bool("unchanged", false, "unchanged bool");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--changed", "--settrue", "--setfalse=false"}));
    EXPECT_TRUE(f->Changed("changed"));
    EXPECT_TRUE(f->Changed("settrue"));
    EXPECT_TRUE(f->Changed("setfalse"));
    EXPECT_FALSE(f->Changed("unchanged"));
    EXPECT_FALSE(f->Changed("invalid"));
    EXPECT_EQ(f->ArgsLenAtDash(), -1);
}

namespace {

pflag::NormalizedName wordSepNormalizeFunc(pflag::FlagSet*, std::string_view name) {
    std::string s(name);
    for (char& c : s) {
        if (c == '-' || c == '_') c = '.';
    }
    return string(s);
}

void testWordSepNormalizedNames(std::vector<string> args) {
    auto f = pflag::NewFlagSet("normalized", pflag::ContinueOnError);
    bool* withDashFlag = f->Bool("with-dash-flag", false, "bool value");
    // Set this after some flags have been added and before others.
    f->SetNormalizeFunc(wordSepNormalizeFunc);
    bool* withUnderFlag = f->Bool("with_under_flag", false, "bool value");
    bool* withBothFlag = f->Bool("with-both_flag", false, "bool value");
    ASSERT_FALSE(f->Parse(args));
    EXPECT_TRUE(*withDashFlag);
    EXPECT_TRUE(*withUnderFlag);
    EXPECT_TRUE(*withBothFlag);
}

}  // namespace

TEST(GoPflag, WordSepNormalizedNames) {
    testWordSepNormalizedNames({"--with-dash-flag", "--with-under-flag", "--with-both-flag"});
    testWordSepNormalizedNames({"--with_dash_flag", "--with_under_flag", "--with_both_flag"});
    testWordSepNormalizedNames({"--with-dash_flag", "--with-under_flag", "--with-both_flag"});
}

TEST(GoPflag, NormalizationFuncShouldChangeFlagName) {
    // Test normalization after addition.
    auto f = pflag::NewFlagSet("normalized", pflag::ContinueOnError);
    f->Bool("valid_flag", false, "bool value");
    ASSERT_TRUE(f->Lookup("valid_flag"));
    EXPECT_EQ(f->Lookup("valid_flag")->Name, "valid_flag");
    f->SetNormalizeFunc(wordSepNormalizeFunc);
    EXPECT_EQ(f->Lookup("valid_flag")->Name, "valid.flag");

    // Test normalization before addition.
    f = pflag::NewFlagSet("normalized", pflag::ContinueOnError);
    f->SetNormalizeFunc(wordSepNormalizeFunc);
    f->Bool("valid_flag", false, "bool value");
    EXPECT_EQ(f->Lookup("valid_flag")->Name, "valid.flag");
}

TEST(GoPflag, UserDefined) {
    auto flags = pflag::NewFlagSet("test", pflag::ContinueOnError);
    flags->SortFlags = false;
    flags->SetOutput(make_ptr<bytes::Buffer>());
    // A Value counting its Sets, like Go's flagVar.
    struct flagVar final : pflag::Value {
        std::vector<string> v;
        string String() const override { return fmt::Sprint("[{}]", strings::Join(v, " ")); }
        error Set(string s) override {
            v.push_back(s);
            return nullptr;
        }
        string Type() const override { return "flagVar"_s; }
    };
    auto v = make_ptr<flagVar>();
    flags->VarP(v, "v", "v", "usage");
    ASSERT_FALSE(flags->Parse(std::vector<string>{"--v=1", "-v2", "-v", "3"}));
    ASSERT_EQ(v->v.size(), 3u);
    EXPECT_EQ(v->String(), "[1 2 3]");
}

TEST(GoPflag, Output) {
    pflag::FlagSet flags("", pflag::ContinueOnError);
    auto buf = make_ptr<bytes::Buffer>();
    std::string expect = "an example string";
    flags.SetOutput(buf);
    fmt::Print(flags.Output(), "{}", expect);
    EXPECT_NE(buf->String().find(expect), string::npos);
}

TEST(GoPflag, NoInterspersed) {
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->SetInterspersed(false);
    f->Bool("true", true, "always true");
    f->Bool("false", false, "always false");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--true", "break", "--false"}));
    EXPECT_EQ(f->Args(), (std::vector<string>{"break", "--false"})) << "expected interspersed options/non-options to fail";
}

TEST(GoPflag, Termination) {
    auto f = pflag::NewFlagSet("termination", pflag::ContinueOnError);
    bool* boolFlag = f->BoolP("bool", "l", false, "bool value");
    f->SetOutput(make_ptr<bytes::Buffer>());
    ASSERT_FALSE(f->Parse(std::vector<string>{"--", "ls", "-l"}));
    EXPECT_FALSE(*boolFlag);
    EXPECT_EQ(f->Args(), (std::vector<string>{"ls", "-l"}));
    EXPECT_EQ(f->ArgsLenAtDash(), 0);
}

TEST(GoPflag, DeprecatedFlagInDocs) {
    auto f = pflag::NewFlagSet("bob", pflag::ContinueOnError);
    f->Bool("badflag", true, "always true");
    f->MarkDeprecated("badflag", "use --good-flag instead");
    auto out = make_ptr<bytes::Buffer>();
    f->SetOutput(out);
    f->PrintDefaults();
    EXPECT_EQ(out->String().find("badflag"), string::npos) << "found deprecated flag in usage!";
}

TEST(GoPflag, DeprecatedFlagShorthandInDocs) {
    auto f = pflag::NewFlagSet("bob", pflag::ContinueOnError);
    f->BoolP("noshorthandflag", "n", true, "always true");
    f->MarkShorthandDeprecated("noshorthandflag", "use --noshorthandflag instead");
    auto out = make_ptr<bytes::Buffer>();
    f->SetOutput(out);
    f->PrintDefaults();
    EXPECT_EQ(out->String().find("-n,"), string::npos) << "found deprecated flag shorthand in usage!";
}

TEST(GoPflag, DeprecatedFlagUsage) {
    auto f = pflag::NewFlagSet("bob", pflag::ContinueOnError);
    f->Bool("badflag", true, "always true");
    std::string usageMsg = "use --good-flag instead";
    f->MarkDeprecated("badflag", usageMsg);
    auto out = make_ptr<bytes::Buffer>();
    f->SetOutput(out);
    ASSERT_FALSE(f->Parse(std::vector<string>{"--badflag"}));
    EXPECT_NE(out->String().find(usageMsg), string::npos) << "usageMsg not printed when using a deprecated flag!";
}

TEST(GoPflag, DeprecatedFlagShorthandUsage) {
    auto f = pflag::NewFlagSet("bob", pflag::ContinueOnError);
    f->BoolP("noshorthandflag", "n", true, "always true");
    std::string usageMsg = "use --noshorthandflag instead";
    f->MarkShorthandDeprecated("noshorthandflag", usageMsg);
    auto out = make_ptr<bytes::Buffer>();
    f->SetOutput(out);
    ASSERT_FALSE(f->Parse(std::vector<string>{"-n"}));
    EXPECT_NE(out->String().find(usageMsg), string::npos) << "usageMsg not printed when using a deprecated flag!";
}

TEST(GoPflag, HiddenFlagInUsage) {
    auto f = pflag::NewFlagSet("bob", pflag::ContinueOnError);
    f->Bool("secretFlag", true, "shhh");
    f->MarkHidden("secretFlag");
    auto out = make_ptr<bytes::Buffer>();
    f->SetOutput(out);
    f->PrintDefaults();
    EXPECT_EQ(out->String().find("secretFlag"), string::npos) << "found hidden flag in usage!";
}

TEST(GoPflag, HiddenFlagUsage) {
    auto f = pflag::NewFlagSet("bob", pflag::ContinueOnError);
    f->Bool("secretFlag", true, "shhh");
    f->MarkHidden("secretFlag");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--secretFlag"})) << "expected no error";
}

TEST(GoPflag, PrintDefaults) {
    auto fs = pflag::NewFlagSet("print defaults test", pflag::ContinueOnError);
    auto buf = make_ptr<bytes::Buffer>();
    fs->SetOutput(buf);
    fs->Bool("A", false, "for bootstrapping, allow 'any' type");
    fs->Bool("Alongflagname", false, "disable bounds checking");
    fs->BoolP("CCC", "C", true, "a boolean defaulting to true");
    fs->String("D", "", "set relative `path` for local imports");
    fs->Float64("F", 2.7, "a non-zero `number`");
    fs->Float64("G", 0, "a float that defaults to zero");
    fs->Int("N", 27, "a non-zero int");
    fs->IntSlice("Ints", {}, "int slice with zero default");
    fs->IP("IP", {}, "IP address with no default");
    fs->IPMask("IPMask", {}, "Netmask address with no default");
    fs->IPNet("IPNet", {}, "IP network with no default");
    fs->Int("Z", 0, "an int that defaults to zero");
    fs->Duration("maxT", {}, "set `timeout` for dial");
    fs->String("ND1", "foo", "a string with NoOptDefVal");
    fs->Lookup("ND1")->NoOptDefVal = "bar";
    fs->Int("ND2", 1234, "a `num` with NoOptDefVal");
    fs->Lookup("ND2")->NoOptDefVal = "4321";
    fs->IntP("EEE", "E", 4321, "a `num` with NoOptDefVal");
    fs->ShorthandLookup("E")->NoOptDefVal = "1234";
    fs->StringSlice("StringSlice", {}, "string slice with zero default");
    fs->StringArray("StringArray", {}, "string array with zero default");
    fs->CountP("verbose", "v", "verbosity");
    fs->Var(make_ptr<customValue>(), "custom", "custom Value implementation");
    fs->VarP(make_ptr<customValue>(10), "customP", "", "a VarP with default");
    // Simulate case where a value has been provided and the help screen is
    // shown.
    fs->Var(make_ptr<customValue>(), "custom-with-val", "custom value which has been set from command line while help is shown");
    EXPECT_FALSE(fs->Parse(std::vector<string>{"--custom-with-val", "3"}));
    fs->PrintDefaults();
    std::string defaultOutput = R"(      --A                         for bootstrapping, allow 'any' type
      --Alongflagname             disable bounds checking
  -C, --CCC                       a boolean defaulting to true (default true)
      --D path                    set relative path for local imports
  -E, --EEE num[=1234]            a num with NoOptDefVal (default 4321)
      --F number                  a non-zero number (default 2.7)
      --G float                   a float that defaults to zero
      --IP ip                     IP address with no default
      --IPMask ipMask             Netmask address with no default
      --IPNet ipNet               IP network with no default
      --Ints ints                 int slice with zero default
      --N int                     a non-zero int (default 27)
      --ND1 string[="bar"]        a string with NoOptDefVal (default "foo")
      --ND2 num[=4321]            a num with NoOptDefVal (default 1234)
      --StringArray stringArray   string array with zero default
      --StringSlice strings       string slice with zero default
      --Z int                     an int that defaults to zero
      --custom custom             custom Value implementation
      --custom-with-val custom    custom value which has been set from command line while help is shown
      --customP custom            a VarP with default (default 10)
      --maxT timeout              set timeout for dial
  -v, --verbose count             verbosity
)";
    EXPECT_EQ(buf->String(), defaultOutput);
}

TEST(GoPflag, VisitAllFlagOrder) {
    auto fs = pflag::NewFlagSet("TestVisitAllFlagOrder", pflag::ContinueOnError);
    fs->SortFlags = false;
    // https://github.com/spf13/pflag/issues/120
    fs->SetNormalizeFunc([](pflag::FlagSet*, std::string_view name) { return string(name); });
    std::vector<string> names = {"C", "B", "A", "D"};
    for (auto& name : names) fs->Bool(name.view(), false, "");
    std::size_t i = 0;
    fs->VisitAll([&](pflag::Flag* f) {
        EXPECT_EQ(names[i], f->Name) << "Incorrect order";
        i++;
    });
}

TEST(GoPflag, VisitFlagOrder) {
    auto fs = pflag::NewFlagSet("TestVisitFlagOrder", pflag::ContinueOnError);
    fs->SortFlags = false;
    std::vector<string> names = {"C", "B", "A", "D"};
    for (auto& name : names) {
        fs->Bool(name.view(), false, "");
        fs->Set(name.view(), "true");
    }
    std::size_t i = 0;
    fs->Visit([&](pflag::Flag* f) {
        EXPECT_EQ(names[i], f->Name) << "Incorrect order";
        i++;
    });
}

TEST(GoPflag, WrapUsage) {
    auto fs = pflag::NewFlagSet("wrap", pflag::ContinueOnError);
    fs->String("name", "", "a flag with a long usage message that is going to be wrapped at some point because it does not fit");
    EXPECT_EQ(fs->FlagUsagesWrapped(60),
              "      --name string   a flag with a long usage message\n                      that is going to be wrapped at\n"
              "                      some point because it does not fit\n");
    // Too narrow to wrap.
    EXPECT_EQ(fs->FlagUsagesWrapped(30),
              "      --name string   a flag with a long usage message that is going to be wrapped at some point because it does not "
              "fit\n");
}

// --- The value types

TEST(GoPflag, SSWithComma) {
    std::vector<string> ss;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->StringSliceVar(&ss, "ss", {}, "Command separated list!");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--ss=\"one,two\"", "--ss=\"three\"", "--ss=\"four,five\",six"}));
    std::vector<string> expected = {"one,two", "three", "four,five", "six"};
    EXPECT_EQ(ss, expected);
    auto [values, err] = f->GetStringSlice("ss");
    EXPECT_FALSE(err) << err;
    EXPECT_EQ(values, expected);
    EXPECT_EQ(f->Lookup("ss")->Value->String(), "[\"one,two\",three,\"four,five\",six]");
}

TEST(GoPflag, SSAsSliceValue) {
    std::vector<string> ss;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->StringSliceVar(&ss, "ss", {}, "Command separated list!");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--ss=one", "--ss=two"}));
    f->VisitAll([](pflag::Flag* flag) {
        if (auto* val = dynamic_cast<pflag::SliceValue*>(flag->Value.get())) val->Replace({"three"});
    });
    EXPECT_EQ(ss, std::vector<string>{"three"}) << "Expected ss to be overwritten with 'three'";
}

TEST(GoPflag, StringArray) {
    std::vector<string> sa;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->StringArrayVarP(&sa, "sa", "s", {"default"}, "array");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--sa=one,two", "-s", "three"}));
    EXPECT_EQ(sa, (std::vector<string>{"one,two", "three"}));
    EXPECT_EQ(f->GetStringArray("sa").first, sa);
    EXPECT_EQ(f->Lookup("sa")->DefValue, "[default]");
}

TEST(GoPflag, IntSlice) {
    std::vector<isize> is;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->IntSliceVar(&is, "is", {}, "ints");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--is=1,2", "--is=3"}));
    EXPECT_EQ(is, (std::vector<isize>{1, 2, 3}));
    EXPECT_EQ(f->Lookup("is")->Value->String(), "[1,2,3]");
    auto err = f->Parse(std::vector<string>{"--is=1,x"});
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "invalid argument \"1,x\" for \"--is\" flag: strconv.Atoi: parsing \"x\": invalid syntax");
}

TEST(GoPflag, S2SCalledTwice) {
    std::map<string, string> s2s;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->StringToStringVar(&s2s, "s2s", {}, "Command separated ls2st!");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--s2s=a=1,b=2", "--s2s=b=3", "--s2s=\"e=5,6\"", "--s2s=f=7,8"}));
    std::map<string, string> expected = {{"a", "1"}, {"b", "3"}, {"e", "5,6"}, {"f", "7,8"}};
    EXPECT_EQ(s2s, expected);
    EXPECT_EQ(f->Lookup("s2s")->Value->String(), "[a=1,b=3,\"e=5,6\",\"f=7,8\"]");
    EXPECT_EQ(f->GetStringToString("s2s").first, expected);
}

TEST(GoPflag, StringToInt) {
    std::map<string, isize> s2i;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->StringToIntVar(&s2i, "s2i", {}, "");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--s2i=a=1,b=2", "--s2i=c=3"}));
    EXPECT_EQ(s2i, (std::map<string, isize>{{"a", 1}, {"b", 2}, {"c", 3}}));
    EXPECT_EQ(f->Lookup("s2i")->Value->String(), "[a=1,b=2,c=3]");
    EXPECT_TRUE(f->Parse(std::vector<string>{"--s2i=a"}));
}

TEST(GoPflag, Count) {
    struct {
        std::vector<string> input;
        bool success;
        isize expected;
    } testCases[] = {
        {{}, true, 0},
        {{"-v"}, true, 1},
        {{"-vvv"}, true, 3},
        {{"-v", "-v", "-v"}, true, 3},
        {{"-v", "--verbose", "-v"}, true, 3},
        {{"-v=3", "-v"}, true, 4},
        {{"--verbose=0"}, true, 0},
        {{"-v=0"}, true, 0},
        {{"-v=a"}, false, 0},
    };
    for (auto& tc : testCases) {
        isize count = 0;
        auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
        f->CountVarP(&count, "verbose", "v", "a counter");
        auto err = f->Parse(tc.input);
        EXPECT_EQ(!err, tc.success) << err;
        if (tc.success) {
            auto [c, cerr] = f->GetCount("verbose");
            EXPECT_FALSE(cerr);
            EXPECT_EQ(c, tc.expected);
        }
    }
}

TEST(GoPflag, BoolSlice) {
    std::vector<bool> bs;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->BoolSliceVar(&bs, "bs", {}, "");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--bs=true,'false'", "--bs= 1 "}));
    EXPECT_EQ(bs, (std::vector<bool>{true, false, true}));
    EXPECT_EQ(f->Lookup("bs")->Value->String(), "[true,false,true]");
}

TEST(GoPflag, FloatFormats) {
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->Float32("f32", 0.1f, "");
    f->Float64("f64", 1e21, "");
    f->Float64Slice("fs", {1.5, 2}, "");
    EXPECT_EQ(f->Lookup("f32")->DefValue, "0.1");
    EXPECT_EQ(f->Lookup("f64")->DefValue, "1e+21");
    EXPECT_EQ(f->Lookup("fs")->DefValue, "[1.500000,2.000000]");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--f32=3.25", "--f64=-0.5"}));
    EXPECT_EQ(f->GetFloat32("f32").first, 3.25f);
    EXPECT_EQ(f->GetFloat64("f64").first, -0.5);
}

TEST(GoPflag, IPNet) {
    net::IPNet n;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->IPNetVar(&n, "net", net::IPNet(net::ParseIP("0.0.0.0"), net::CIDRMask(0, 32)), "");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--net=192.168.1.10/24"}));
    EXPECT_EQ(n.String(), "192.168.1.0/24");
    auto err = f->Parse(std::vector<string>{"--net=bad"});
    ASSERT_TRUE(err);
}

TEST(GoPflag, IPMask) {
    EXPECT_EQ(pflag::ParseIPv4Mask("255.255.255.0").String(), "ffffff00");
    EXPECT_EQ(pflag::ParseIPv4Mask("ffff0000").String(), "ffff0000");
    EXPECT_TRUE(pflag::ParseIPv4Mask("nonsense").isNil());
}

TEST(GoPflag, Bytes) {
    slice<byte> hx, b64;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->BytesHexVar(&hx, "hex", {}, "");
    f->BytesBase64Var(&b64, "b64", {}, "");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--hex=0102ff", "--b64=aGVsbG8="}));
    EXPECT_EQ(f->Lookup("hex")->Value->String(), "0102FF");
    EXPECT_EQ(String(b64), "hello");
    EXPECT_TRUE(f->Parse(std::vector<string>{"--hex=zz"}));
}

TEST(GoPflag, DurationSlice) {
    std::vector<time::Duration> ds;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->DurationSliceVar(&ds, "ds", {}, "");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--ds=1s,2m", "--ds=3h"}));
    EXPECT_EQ(ds, (std::vector<time::Duration>{time::Second, 2 * time::Minute, 3 * time::Hour}));
    EXPECT_EQ(f->Lookup("ds")->Value->String(), "[1s,2m0s,3h0m0s]");
}

TEST(GoPflag, Time) {
    time::Time t;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->TimeVar(&t, "time", {}, {string(time::RFC3339), string(time::DateOnly)}, "");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--time=2024-03-04"}));
    EXPECT_EQ(t.Year(), 2024);
    EXPECT_EQ(f->Lookup("time")->Value->String(), "2024-03-04T00:00:00Z");
    auto err = f->Parse(std::vector<string>{"--time=yesterday"});
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(),
              "invalid argument \"yesterday\" for \"--time\" flag: invalid time format `yesterday` must be one of: "
              "`2006-01-02T15:04:05Z07:00`, `2006-01-02`");
}

TEST(GoPflag, Func) {
    std::vector<string> got;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->FuncP("add", "a", "adds", [&](string s) -> error {
        if (s == "bad") return errors::New("no good");
        got.push_back(s);
        return nullptr;
    });
    int called = 0;
    f->BoolFunc("ping", "pings", [&](string s) -> error {
        EXPECT_EQ(s, "true");
        called++;
        return nullptr;
    });
    ASSERT_FALSE(f->Parse(std::vector<string>{"--add=x", "-a", "y", "--ping"}));
    EXPECT_EQ(got, (std::vector<string>{"x", "y"}));
    EXPECT_EQ(called, 1);
    auto err = f->Parse(std::vector<string>{"--add=bad"});
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "invalid argument \"bad\" for \"-a, --add\" flag: no good");
    EXPECT_EQ(f->FlagUsages(), "  -a, --add value   adds\n      --ping        pings\n");
}

TEST(GoPflag, Text) {
    netip::Addr addr;
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->TextVar(&addr, "addr", netip::MustParseAddr("127.0.0.1"), "an address");
    EXPECT_EQ(f->Lookup("addr")->DefValue, "127.0.0.1");
    ASSERT_FALSE(f->Parse(std::vector<string>{"--addr=::1"}));
    EXPECT_EQ(addr.String(), "::1");
    netip::Addr out;
    EXPECT_FALSE(f->GetText("addr", &out));
    EXPECT_EQ(out, addr);
}

TEST(GoPflag, ErrorTypes) {
    auto f = pflag::NewFlagSet("test", pflag::ContinueOnError);
    f->IntP("num", "n", 0, "");
    auto err = f->Parse(std::vector<string>{"-n", "x"});
    ASSERT_TRUE(err);
    auto* iv = errors::As<pflag::InvalidValueError>(err);
    ASSERT_TRUE(iv);
    EXPECT_EQ(iv->GetValue(), "x");
    EXPECT_EQ(iv->GetFlag()->Name, "num");
    EXPECT_TRUE(errors::Is(err, strconv::ErrSyntax));

    err = f->Parse(std::vector<string>{"--nope"});
    auto* ne = errors::As<pflag::NotExistError>(err);
    ASSERT_TRUE(ne);
    EXPECT_EQ(ne->GetSpecifiedName(), "nope");

    err = f->Parse(std::vector<string>{"---x"});
    ASSERT_TRUE(errors::As<pflag::InvalidSyntaxError>(err));
    EXPECT_EQ(err.Error(), "bad flag syntax: ---x");

    err = f->Parse(std::vector<string>{"--help"});
    EXPECT_EQ(err, pflag::ErrHelp);
}
