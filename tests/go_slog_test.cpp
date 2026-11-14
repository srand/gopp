// Ported from Go 1.27.3's log/slog tests (value_test.go, level_test.go,
// record_test.go, handler_test.go, text_handler_test.go,
// json_handler_test.go, logger_test.go and multi_handler_test.go), with
// Go's test names.
//
// Left out: the allocation tests and benchmarks; the flag.TextVar tests
// (go++'s flag has no TextVar); the tests of recovering from panics in
// LogValue, MarshalText and MarshalJSON, which go++ doesn't recover; the
// json.RawMessage case, which go++'s json doesn't have; and the "!BUG"
// check of TestAliasingAndClone, since copies of a Record never share
// their attributes in C++. Sources are std::source_locations: where Go
// checks a function's name, these check the name source_location gives.
#include <gopp/context.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/io.hpp>
#include <gopp/log.hpp>
#include <gopp/log/slog.hpp>
#include <gopp/os.hpp>
#include <gopp/path.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include "log/slog/internal.hpp"
#include "text/template/goconv.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <regex>
#include <string>
#include <vector>

using namespace gopp;
using slog::Attr;
using slog::Value;

namespace {

const time::Time testTime = time::Date(2000, time::January, 2, 3, 4, 5, 0, time::UTC);

const std::string marshalerASCII = [] {
    std::string b(128, '\0');
    for (int i = 0; i < 128; i++) b[static_cast<std::size_t>(i)] = static_cast<char>(i);
    return b;
}();

// textTimeRE matches the text handler's timestamps: RFC3339Nano with
// exactly three fractional digits.
const std::string textTimeRE = R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}(Z|[+-]\d{2}:\d{2}))";
// jsonTimeRE matches the JSON handler's: RFC3339Nano.
const std::string jsonTimeRE = R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2}))";

std::string quote(std::string_view s) { return text::tmpl::detail::quote(s); }

std::vector<Attr> attrsSlice(const slog::Record& r) {
    std::vector<Attr> s;
    r.Attrs([&](const Attr& a) {
        s.push_back(a);
        return true;
    });
    return s;
}

bool attrsEqual(std::span<const Attr> as1, std::span<const Attr> as2) {
    return std::equal(as1.begin(), as1.end(), as2.begin(), as2.end(),
                      [](const Attr& a, const Attr& b) { return a.Equal(b); });
}

std::string str(std::span<const Attr> as) {
    std::string s = "[";
    for (std::size_t i = 0; i < as.size(); i++) {
        if (i > 0) s += " ";
        s += as[i].String().view();
    }
    return s + "]";
}

// clean prepares log output for comparison.
std::string clean(std::string s) {
    if (!s.empty() && s.back() == '\n') s.pop_back();
    std::replace(s.begin(), s.end(), '\n', '~');
    return s;
}

#define checkLogOutput(got, wantRegexp)                                                        \
    do {                                                                                       \
        std::string got_ = clean(std::string(got));                                            \
        std::string want_ = (wantRegexp);                                                      \
        EXPECT_TRUE(std::regex_match(got_, std::regex(want_))) << "got  " << got_ << "\nwant " \
                                                               << want_;                       \
    } while (0)

// removeKeys returns a function suitable for HandlerOptions.ReplaceAttr
// that removes all Attrs with the given keys.
std::function<Attr(std::span<const string>, Attr)> removeKeys(std::vector<std::string_view> keys) {
    return [keys](std::span<const string>, Attr a) {
        for (auto k : keys) {
            if (a.Key == k) return Attr{};
        }
        return a;
    };
}

Attr upperCaseKey(std::span<const string>, Attr a) {
    a.Key = strings::ToUpper(a.Key);
    return a;
}

ptr<strings::Builder> newBuffer() { return make_ptr<strings::Builder>(); }

struct logValueName {
    std::string first, last;
    Value LogValue() const { return slog::GroupValue(slog::String("first", first), slog::String("last", last)); }
};

struct replace : object {
    explicit replace(Value v) : v(std::move(v)) {}
    Value v;
    Value LogValue() const { return v; }
};

// for testing fmt.Sprint
struct name {
    std::string First, Last;
    std::string String() const { return Last + ", " + First; }
};

// for testing TextMarshaler
struct text_ {
    std::string s;
    std::string String() const { return s; }  // should be ignored
    std::pair<slice<byte>, error> MarshalText() const {
        if (s.empty()) return {slice<byte>(), errors::New("text: empty string")};
        return {toBytes("text{" + quote(s) + "}"), nullptr};
    }
};

// for testing TextAppender
struct textAppend {
    std::string s;
    std::string String() const { return s; }
    std::pair<slice<byte>, error> MarshalText() const { return {toBytes(s), nullptr}; }
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const {
        if (s.empty()) return {slice<byte>(), errors::New("textAppend: empty string")};
        b = appendSpan(std::move(b), as_bytes(std::string_view("textAppend{\"")));
        b = appendSpan(std::move(b), as_bytes(std::string_view(s)));
        b = appendSpan(std::move(b), as_bytes(std::string_view("\"}")));
        return {b, nullptr};
    }
};

// for testing json.Marshaler
struct jsonMarshaler {
    std::string s;
    std::string String() const { return s; }  // should be ignored
    std::pair<slice<byte>, error> MarshalJSON() const {
        if (s.empty()) return {slice<byte>(), errors::New("json: empty string")};
        return json::Marshal(std::vector<std::string>{s});
    }
};

struct jsonMarshalerError : jsonMarshaler {
    std::string Error() const { return "oops"; }
};

struct captureHandler : slog::Handler {
    sync::Mutex mu;
    ptr<context::Context> ctx;
    slog::Record r;
    std::vector<Attr> attrs;
    std::vector<string> groups;

    error Handle(const ptr<context::Context>& c, const slog::Record& rec) override {
        std::lock_guard lock(mu);
        ctx = c;
        r = rec;
        return nullptr;
    }
    bool Enabled(const ptr<context::Context>&, slog::Level) override { return true; }
    ptr<slog::Handler> WithAttrs(std::span<const Attr> as) override {
        std::lock_guard lock(mu);
        auto c2 = make_ptr<captureHandler>();
        c2->r = r;
        c2->groups = groups;
        c2->attrs = attrs;
        c2->attrs.insert(c2->attrs.end(), as.begin(), as.end());
        return c2;
    }
    ptr<slog::Handler> WithGroup(std::string_view name) override {
        std::lock_guard lock(mu);
        auto c2 = make_ptr<captureHandler>();
        c2->r = r;
        c2->attrs = attrs;
        c2->groups = groups;
        c2->groups.emplace_back(name);
        return c2;
    }
    void clear() {
        std::lock_guard lock(mu);
        ctx = nullptr;
        r = slog::Record();
    }
};

struct wrappingHandler : slog::Handler {
    explicit wrappingHandler(ptr<slog::Handler> h) : h(std::move(h)) {}
    ptr<slog::Handler> h;
    bool Enabled(const ptr<context::Context>& ctx, slog::Level level) override { return h->Enabled(ctx, level); }
    ptr<slog::Handler> WithGroup(std::string_view name) override { return h->WithGroup(name); }
    ptr<slog::Handler> WithAttrs(std::span<const Attr> as) override { return h->WithAttrs(as); }
    error Handle(const ptr<context::Context>& ctx, const slog::Record& r) override { return h->Handle(ctx, r); }
};

// The handler of log's standard logger, which slog's default Logger has.
ptr<slog::Handler> logDefaultHandler() {
    return slog::detail::newDefaultHandler(
        [](const std::source_location& loc, std::string_view s) { return log::Default()->output(loc, s); });
}

// Restores the default Loggers of slog and log at the end of a test.
struct restoreDefaults {
    ptr<slog::Logger> logger = slog::Default();
    ptr<io::Writer> writer = log::Writer();
    int flags = log::Flags();
    slog::Level level = slog::SetLogLoggerLevel(slog::LevelInfo);
    restoreDefaults() { slog::SetLogLoggerLevel(level); }
    ~restoreDefaults() {
        slog::SetDefault(logger);
        slog::SetLogLoggerLevel(level);
        log::SetOutput(writer);
        log::SetFlags(flags);
    }
};

// ---------------------------------------------------------------------------
// value_test.go

TEST(GoSlog, KindString) { EXPECT_EQ(slog::KindGroup.String(), "Group"); }

TEST(GoSlog, ValueEqual) {
    int x = 0, y = 0;
    std::vector<Value> vals = {
        Value(),
        slog::Int64Value(1),
        slog::Int64Value(2),
        slog::Float64Value(3.5),
        slog::Float64Value(3.7),
        slog::BoolValue(true),
        slog::BoolValue(false),
        slog::TimeValue(testTime),
        slog::TimeValue(time::Time()),
        slog::TimeValue(time::Date(2001, time::January, 2, 3, 4, 5, 0, time::UTC)),
        slog::TimeValue(time::Date(2300, time::January, 1, 0, 0, 0, 0, time::UTC)),           // overflows nanoseconds
        slog::TimeValue(time::Date(1715, time::June, 13, 0, 25, 26, 290448384, time::UTC)),  // overflowed value
        slog::AnyValue(&x),
        slog::AnyValue(&y),
        slog::GroupValue(slog::Bool("b", true), slog::Int("i", 3)),
        slog::GroupValue(slog::Bool("b", true), slog::Int("i", 4)),
        slog::GroupValue(slog::Bool("b", true), slog::Int("j", 4)),
        slog::DurationValue(3 * time::Second),
        slog::DurationValue(2 * time::Second),
        slog::StringValue("foo"),
        slog::StringValue("fuu"),
    };
    for (std::size_t i = 0; i < vals.size(); i++) {
        for (std::size_t j = 0; j < vals.size(); j++) {
            EXPECT_EQ(vals[i].Equal(vals[j]), i == j) << vals[i] << ".Equal(" << vals[j] << ")";
        }
    }
}

TEST(GoSlog, ValueString) {
    struct {
        Value v;
        std::string want;
    } tests[] = {
        {slog::Int64Value(-3), "-3"},
        {slog::Uint64Value(1), "1"},
        {slog::Float64Value(.15), "0.15"},
        {slog::BoolValue(true), "true"},
        {slog::StringValue("foo"), "foo"},
        {slog::TimeValue(testTime), "2000-01-02 03:04:05 +0000 UTC"},
        {slog::AnyValue(time::Duration(3 * time::Second)), "3s"},
        {slog::GroupValue(slog::Int("a", 1), slog::Bool("b", true)), "[a=1 b=true]"},
    };
    for (const auto& test : tests) EXPECT_EQ(test.v.String(), test.want);
}

TEST(GoSlog, AnyValue) {
    struct {
        Value got;
        Value want;
    } tests[] = {
        {slog::AnyValue(1), slog::IntValue(1)},
        {slog::AnyValue(1.5), slog::Float64Value(1.5)},
        {slog::AnyValue(2.5f), slog::Float64Value(2.5)},
        {slog::AnyValue("s"), slog::StringValue("s")},
        {slog::AnyValue(true), slog::BoolValue(true)},
        {slog::AnyValue(testTime), slog::TimeValue(testTime)},
        {slog::AnyValue(time::Hour), slog::DurationValue(time::Hour)},
        {slog::AnyValue(std::vector<Attr>{slog::Int("i", 3)}), slog::GroupValue(slog::Int("i", 3))},
        {slog::AnyValue(slog::IntValue(4)), slog::IntValue(4)},
        {slog::AnyValue(2u), slog::Uint64Value(2)},
        {slog::AnyValue(uint8(3)), slog::Uint64Value(3)},
        {slog::AnyValue(uint16(4)), slog::Uint64Value(4)},
        {slog::AnyValue(uint32(5)), slog::Uint64Value(5)},
        {slog::AnyValue(uint64(6)), slog::Uint64Value(6)},
        {slog::AnyValue(uintptr(7)), slog::Uint64Value(7)},
        {slog::AnyValue(int8(8)), slog::Int64Value(8)},
        {slog::AnyValue(int16(9)), slog::Int64Value(9)},
        {slog::AnyValue(int32(10)), slog::Int64Value(10)},
        {slog::AnyValue(int64(11)), slog::Int64Value(11)},
    };
    for (const auto& test : tests) {
        EXPECT_TRUE(test.got.Equal(test.want)) << "got " << test.got << " (kind " << test.got.Kind() << "), want "
                                               << test.want << " (kind " << test.want.Kind() << ")";
    }
}

TEST(GoSlog, ValueAny) {
    EXPECT_FALSE(slog::AnyValue(nullptr).Any().has_value());
    EXPECT_EQ(std::any_cast<slog::Level>(slog::AnyValue(slog::LevelDebug + 100).Any()), slog::LevelDebug + 100);
    EXPECT_EQ(std::any_cast<const time::Location*>(slog::AnyValue(time::UTC).Any()), time::UTC);
    EXPECT_EQ(std::any_cast<slog::Kind>(slog::AnyValue(slog::KindBool).Any()), slog::KindBool);
    std::vector<Attr> as{slog::Int("a", 1)};
    EXPECT_TRUE(attrsEqual(std::any_cast<std::vector<Attr>>(slog::AnyValue(as).Any()), as));
    EXPECT_EQ(std::any_cast<int64>(slog::AnyValue(int64(2)).Any()), 2);
    EXPECT_EQ(std::any_cast<uint64>(slog::AnyValue(uint64(3)).Any()), 3u);
    EXPECT_EQ(std::any_cast<bool>(slog::AnyValue(true).Any()), true);
    EXPECT_EQ(std::any_cast<time::Duration>(slog::AnyValue(time::Minute).Any()), time::Minute);
    EXPECT_TRUE(std::any_cast<time::Time>(slog::AnyValue(time::Time()).Any()).IsZero());
    EXPECT_EQ(std::any_cast<double>(slog::AnyValue(3.14).Any()), 3.14);
    EXPECT_EQ(std::any_cast<string>(slog::AnyValue("foo").Any()), "foo");
}

TEST(GoSlog, LogValue) {
    const std::string want = "replaced";
    auto r = make_ptr<replace>(slog::StringValue(want));
    Value v = slog::AnyValue(r);
    EXPECT_EQ(v.Kind(), slog::KindLogValuer);
    EXPECT_EQ(std::any_cast<string>(v.LogValuer().LogValue().Any()), want);

    // Test Resolve.
    EXPECT_EQ(std::any_cast<string>(v.Resolve().Any()), want);

    // Test Resolve max iteration.
    r->v = slog::AnyValue(r);  // create a cycle
    auto got = slog::AnyValue(r).Resolve().Any();
    EXPECT_NE(std::any_cast<error>(&got), nullptr) << "expected error, got " << got.type().name();
    r->v = Value();  // break the cycle

    // Groups are not recursively resolved.
    Attr c = slog::Any("c", make_ptr<replace>(slog::StringValue("d")));
    v = slog::AnyValue(make_ptr<replace>(slog::GroupValue(slog::Int("a", 1), slog::Group("b", c))));
    auto got2 = std::any_cast<std::vector<Attr>>(v.Resolve().Any());
    std::vector<Attr> want2 = {slog::Int("a", 1), slog::Group("b", c)};
    EXPECT_TRUE(attrsEqual(got2, want2)) << "got " << str(got2) << ", want " << str(want2);
}

TEST(GoSlog, ValueTime) {
    // Validate that all representations of times work correctly.
    for (const time::Time& tm : {
             time::Time(),
             time::Unix(0, 1'000'000'000'000'000),                          // UnixNanos is defined
             time::Date(2300, time::January, 1, 0, 0, 0, 0, time::UTC),  // overflows UnixNanos
         }) {
        auto got = slog::TimeValue(tm).Time();
        EXPECT_TRUE(got.Equal(tm)) << "got " << got.String() << ", want " << tm.String();
        EXPECT_EQ(got.Location(), tm.Location()) << tm.String();
    }
}

TEST(GoSlog, EmptyGroup) {
    auto g = slog::GroupValue(slog::Int("a", 1), slog::Group("g1", slog::Group("g2")),
                              slog::Group("g3", slog::Group("g4", slog::Int("b", 2))));
    auto got = g.Group();
    std::vector<Attr> want = {slog::Int("a", 1), slog::Group("g3", slog::Group("g4", slog::Int("b", 2)))};
    EXPECT_TRUE(attrsEqual(got, want)) << "got " << str(got) << "\nwant " << str(want);
}

// ---------------------------------------------------------------------------
// level_test.go

TEST(GoSlog, LevelString) {
    struct {
        slog::Level in;
        std::string want;
    } tests[] = {
        {slog::Level(0), "INFO"},
        {slog::LevelError, "ERROR"},
        {slog::LevelError + 2, "ERROR+2"},
        {slog::LevelError - 2, "WARN+2"},
        {slog::LevelWarn, "WARN"},
        {slog::LevelWarn - 1, "INFO+3"},
        {slog::LevelInfo, "INFO"},
        {slog::LevelInfo + 1, "INFO+1"},
        {slog::LevelInfo - 3, "DEBUG+1"},
        {slog::LevelDebug, "DEBUG"},
        {slog::LevelDebug - 2, "DEBUG-2"},
    };
    for (const auto& test : tests) EXPECT_EQ(test.in.String(), test.want) << test.in.value;
}

TEST(GoSlog, LevelVar) {
    slog::LevelVar al;
    EXPECT_EQ(al.Level(), slog::LevelInfo);
    al.Set(slog::LevelWarn);
    EXPECT_EQ(al.Level(), slog::LevelWarn);
    al.Set(slog::LevelInfo);
    EXPECT_EQ(al.Level(), slog::LevelInfo);
}

TEST(GoSlog, LevelMarshalJSON) {
    auto want = slog::LevelWarn - 3;
    auto [data, err] = want.MarshalJSON();
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(asStringView(data), R"("INFO+1")");
    slog::Level got;
    ASSERT_FALSE(got.UnmarshalJSON(data));
    EXPECT_EQ(got, want);
}

TEST(GoSlog, LevelMarshalText) {
    auto want = slog::LevelWarn - 3;
    auto [data, err] = want.MarshalText();
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(asStringView(data), "INFO+1");
    slog::Level got;
    ASSERT_FALSE(got.UnmarshalText(data));
    EXPECT_EQ(got, want);
}

TEST(GoSlog, LevelAppendText) {
    auto buf = slice<byte>::make(4, 16);
    auto want = slog::LevelWarn - 3;
    auto [data, err] = want.AppendText(buf);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(asStringView(data), std::string_view("\x00\x00\x00\x00INFO+1", 10));
}

TEST(GoSlog, LevelParse) {
    struct {
        std::string in;
        slog::Level want;
    } tests[] = {
        {"DEBUG", slog::LevelDebug},
        {"INFO", slog::LevelInfo},
        {"WARN", slog::LevelWarn},
        {"ERROR", slog::LevelError},
        {"debug", slog::LevelDebug},
        {"iNfo", slog::LevelInfo},
        {"INFO+87", slog::LevelInfo + 87},
        {"Error-18", slog::LevelError - 18},
        {"Error-8", slog::LevelInfo},
    };
    for (const auto& test : tests) {
        slog::Level got;
        auto err = got.UnmarshalText(as_bytes(test.in));
        ASSERT_FALSE(err) << test.in << ": " << err;
        EXPECT_EQ(got, test.want) << test.in;
    }
}

TEST(GoSlog, LevelParseError) {
    struct {
        std::string in;
        std::string want;  // error string should contain this
    } tests[] = {
        {"", "unknown name"},
        {"dbg", "unknown name"},
        {"INFO+", "invalid syntax"},
        {"INFO-", "invalid syntax"},
        {"ERROR+23x", "invalid syntax"},
    };
    for (const auto& test : tests) {
        slog::Level l;
        auto err = l.UnmarshalText(as_bytes(test.in));
        EXPECT_TRUE(err && strings::Contains(err.Error(), test.want))
            << test.in << ": got " << err << ", want string containing " << test.want;
    }
}

TEST(GoSlog, LevelVarMarshalText) {
    slog::LevelVar v;
    v.Set(slog::LevelWarn);
    auto [data, err] = v.MarshalText();
    ASSERT_FALSE(err) << err;
    slog::LevelVar v2;
    ASSERT_FALSE(v2.UnmarshalText(data));
    EXPECT_EQ(v2.Level(), slog::LevelWarn);
}

TEST(GoSlog, LevelVarAppendText) {
    slog::LevelVar v;
    v.Set(slog::LevelWarn);
    auto buf = slice<byte>::make(4, 16);
    auto [data, err] = v.AppendText(buf);
    ASSERT_FALSE(err) << err;
    slog::LevelVar v2;
    ASSERT_FALSE(v2.UnmarshalText(data.Sub(4)));
    EXPECT_EQ(v2.Level(), slog::LevelWarn);
}

TEST(GoSlog, LevelVarString) {
    slog::LevelVar v;
    v.Set(slog::LevelError);
    EXPECT_EQ(v.String(), "LevelVar(ERROR)");
}

// ---------------------------------------------------------------------------
// record_test.go

slog::Record newRecordWithAttrs(std::span<const Attr> as) {
    auto r = slog::NewRecord(time::Now(), slog::LevelInfo, "", {});
    r.AddAttrs(as);
    return r;
}

TEST(GoSlog, RecordAttrs) {
    std::vector<Attr> as = {slog::Int("k1", 1),     slog::String("k2", "foo"),  slog::Int("k3", 3),
                            slog::Int64("k4", -1), slog::Float64("f", 3.1), slog::Uint64("u", 999)};
    auto r = newRecordWithAttrs(as);
    EXPECT_EQ(r.NumAttrs(), static_cast<int>(as.size()));
    EXPECT_TRUE(attrsEqual(attrsSlice(r), as));

    // Early return.
    // Hit both loops in Record.Attrs: front and back.
    for (std::size_t stop : {2, 6}) {
        std::vector<Attr> got;
        r.Attrs([&](const Attr& a) {
            got.push_back(a);
            return got.size() < stop;
        });
        EXPECT_TRUE(attrsEqual(got, std::span(as).first(stop))) << "got " << str(got);
    }
}

TEST(GoSlog, RecordSource) {
    // No location => no Source.
    auto r = slog::NewRecord(time::Time(), slog::Level(0), "", std::source_location());
    EXPECT_FALSE(r.Source().has_value());

    auto here = std::source_location::current();
    r = slog::NewRecord(time::Time(), slog::Level(0), "", here);
    auto got = r.Source();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->Function, here.function_name());
    EXPECT_EQ(path::Base(got->File), "go_slog_test.cpp");
    EXPECT_GT(got->Line, 0);
}

TEST(GoSlog, AliasingAndClone) {
    auto intAttrs = [](int from, int to) {
        std::vector<Attr> as;
        for (int i = from; i < to; i++) as.push_back(slog::Int("k", i));
        return as;
    };
    auto concat = [](std::vector<Attr> a, std::initializer_list<Attr> b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };

    // Create a record whose Attrs overflow the inline array.
    auto r1 = slog::NewRecord(time::Time(), slog::Level(0), "", {});
    r1.AddAttrs(intAttrs(0, 6));
    // A copy doesn't share state, in C++.
    auto r2 = r1;
    auto r1AttrsBefore = attrsSlice(r1);
    r1.AddAttrs(slog::Int("p", 0));
    r2.AddAttrs(slog::Int("p", 1));
    EXPECT_TRUE(attrsEqual(attrsSlice(r1), concat(r1AttrsBefore, {slog::Int("p", 0)})));
    auto r1Attrs = attrsSlice(r1);
    EXPECT_TRUE(attrsEqual(attrsSlice(r2), concat(r1AttrsBefore, {slog::Int("p", 1)})));

    // Adding to a clone is fine.
    r2 = r1.Clone();
    EXPECT_TRUE(attrsEqual(attrsSlice(r2), r1Attrs));
    r2.AddAttrs(slog::Int("p", 2));
    EXPECT_TRUE(attrsEqual(attrsSlice(r1), r1Attrs));  // r1 is unchanged
    EXPECT_TRUE(attrsEqual(attrsSlice(r2), concat(r1Attrs, {slog::Int("p", 2)})));
}

// ---------------------------------------------------------------------------
// handler_test.go

TEST(GoSlog, DefaultHandle) {
    auto ctx = context::Background();
    std::vector<Attr> preAttrs = {slog::Int("pre", 0)};
    std::vector<Attr> attrs = {slog::Int("a", 1), slog::String("b", "two")};
    using withFn = std::function<ptr<slog::Handler>(ptr<slog::Handler>)>;
    struct {
        std::string name;
        withFn with;
        std::vector<Attr> attrs;
        std::string want;
    } tests[] = {
        {"no attrs", nullptr, {}, "INFO message"},
        {"attrs", nullptr, attrs, "INFO message a=1 b=two"},
        {"preformatted", [&](ptr<slog::Handler> h) { return h->WithAttrs(preAttrs); }, attrs,
         "INFO message pre=0 a=1 b=two"},
        {"groups",
         nullptr,
         {slog::Int("a", 1), slog::Group("g", slog::Int("b", 2), slog::Group("h", slog::Int("c", 3)), slog::Int("d", 4)),
          slog::Int("e", 5)},
         "INFO message a=1 g.b=2 g.h.c=3 g.d=4 e=5"},
        {"group", [&](ptr<slog::Handler> h) { return h->WithAttrs(preAttrs)->WithGroup("s"); }, attrs,
         "INFO message pre=0 s.a=1 s.b=two"},
        {"preformatted groups",
         [](ptr<slog::Handler> h) {
             return h->WithAttrs({{slog::Int("p1", 1)}})->WithGroup("s1")->WithAttrs({{slog::Int("p2", 2)}})->WithGroup("s2");
         },
         attrs, "INFO message p1=1 s1.p2=2 s1.s2.a=1 s1.s2.b=two"},
        {"two with-groups",
         [](ptr<slog::Handler> h) { return h->WithAttrs({{slog::Int("p1", 1)}})->WithGroup("s1")->WithGroup("s2"); },
         attrs, "INFO message p1=1 s1.s2.a=1 s1.s2.b=two"},
    };
    for (const auto& test : tests) {
        std::string got;
        ptr<slog::Handler> h = slog::detail::newDefaultHandler([&](const std::source_location&, std::string_view b) {
            got = b;
            return error();
        });
        if (test.with) h = test.with(h);
        auto r = slog::NewRecord(time::Time(), slog::LevelInfo, "message", {});
        r.AddAttrs(test.attrs);
        ASSERT_FALSE(h->Handle(ctx, r)) << test.name;
        EXPECT_EQ(got, test.want) << test.name;
    }
}

TEST(GoSlog, ConcurrentWrites) {
    auto ctx = context::Background();
    constexpr int count = 1000;
    for (std::string handlerType : {"text", "json"}) {
        auto buf = newBuffer();
        ptr<slog::Handler> h;
        if (handlerType == "text") h = slog::NewTextHandler(buf, nullptr);
        else h = slog::NewJSONHandler(buf, nullptr);
        auto sub1 = h->WithAttrs({{slog::Bool("sub1", true)}});
        auto sub2 = h->WithAttrs({{slog::Bool("sub2", true)}});
        sync::WaitGroup wg;
        for (int i = 0; i < count; i++) {
            auto sub1Record = slog::NewRecord(time::Time(), slog::LevelInfo, "hello from sub1", {});
            sub1Record.AddAttrs(slog::Int("i", i));
            auto sub2Record = slog::NewRecord(time::Time(), slog::LevelInfo, "hello from sub2", {});
            sub2Record.AddAttrs(slog::Int("i", i));
            wg.Add(1);
            go([&, sub1Record, sub2Record] {
                EXPECT_FALSE(sub1->Handle(ctx, sub1Record));
                EXPECT_FALSE(sub2->Handle(ctx, sub2Record));
                wg.Done();
            });
        }
        wg.Wait();
        for (int i = 1; i <= 2; i++) {
            std::string want = "hello from sub" + std::to_string(i);
            std::string_view s = buf->view();
            int n = 0;
            for (auto p = s.find(want); p != s.npos; p = s.find(want, p + 1)) n++;
            EXPECT_EQ(n, count) << handlerType << ": occurrences of " << want;
        }
    }
}

// Verify the common parts of TextHandler and JSONHandler.
TEST(GoSlog, JSONAndTextHandlers) {
    // remove all Attrs
    auto removeAll = [](std::span<const string>, Attr) { return Attr{}; };

    std::vector<Attr> attrs = {slog::String("a", "one"), slog::Int("b", 2), slog::Any("", nullptr)};
    std::vector<Attr> preAttrs = {slog::Int("pre", 3), slog::String("x", "y")};
    using replaceFn = std::function<Attr(std::span<const string>, Attr)>;
    using withFn = std::function<ptr<slog::Handler>(ptr<slog::Handler>)>;
    auto concat = [](std::vector<Attr> a, std::initializer_list<Attr> b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    auto withAttrs = [](std::initializer_list<Attr> as) { return std::vector<Attr>(as); };

    struct testCase {
        std::string name;
        replaceFn replace;
        bool addSource = false;
        withFn with;
        std::vector<Attr> attrs;
        std::string wantText;
        std::string wantJSON;
    };
    std::vector<testCase> tests = {
        {"basic", nullptr, false, nullptr, attrs, "time=2000-01-02T03:04:05.000Z level=INFO msg=message a=one b=2",
         R"({"time":"2000-01-02T03:04:05Z","level":"INFO","msg":"message","a":"one","b":2})"},
        {"empty key", nullptr, false, nullptr, concat(attrs, {slog::Any("", "v")}),
         R"(time=2000-01-02T03:04:05.000Z level=INFO msg=message a=one b=2 ""=v)",
         R"({"time":"2000-01-02T03:04:05Z","level":"INFO","msg":"message","a":"one","b":2,"":"v"})"},
        {"cap keys", upperCaseKey, false, nullptr, attrs, "TIME=2000-01-02T03:04:05.000Z LEVEL=INFO MSG=message A=one B=2",
         R"({"TIME":"2000-01-02T03:04:05Z","LEVEL":"INFO","MSG":"message","A":"one","B":2})"},
        {"remove all", removeAll, false, nullptr, attrs, "", "{}"},
        {"preformatted", nullptr, false, [&](ptr<slog::Handler> h) { return h->WithAttrs(preAttrs); }, attrs,
         "time=2000-01-02T03:04:05.000Z level=INFO msg=message pre=3 x=y a=one b=2",
         R"({"time":"2000-01-02T03:04:05Z","level":"INFO","msg":"message","pre":3,"x":"y","a":"one","b":2})"},
        {"preformatted cap keys", upperCaseKey, false, [&](ptr<slog::Handler> h) { return h->WithAttrs(preAttrs); },
         attrs, "TIME=2000-01-02T03:04:05.000Z LEVEL=INFO MSG=message PRE=3 X=y A=one B=2",
         R"({"TIME":"2000-01-02T03:04:05Z","LEVEL":"INFO","MSG":"message","PRE":3,"X":"y","A":"one","B":2})"},
        {"preformatted remove all", removeAll, false, [&](ptr<slog::Handler> h) { return h->WithAttrs(preAttrs); },
         attrs, "", "{}"},
        {"remove built-in", removeKeys({slog::TimeKey, slog::LevelKey, slog::MessageKey}), false, nullptr, attrs,
         "a=one b=2", R"({"a":"one","b":2})"},
        {"preformatted remove built-in", removeKeys({slog::TimeKey, slog::LevelKey, slog::MessageKey}), false,
         [&](ptr<slog::Handler> h) { return h->WithAttrs(preAttrs); }, attrs, "pre=3 x=y a=one b=2",
         R"({"pre":3,"x":"y","a":"one","b":2})"},
        {"groups", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Int("a", 1),
                    slog::Group("g", slog::Int("b", 2), slog::Group("h", slog::Int("c", 3)), slog::Int("d", 4)),
                    slog::Int("e", 5)}),
         "msg=message a=1 g.b=2 g.h.c=3 g.d=4 e=5", R"({"msg":"message","a":1,"g":{"b":2,"h":{"c":3},"d":4},"e":5})"},
        {"empty group", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Group("g"), slog::Group("h", slog::Int("a", 1))}), "msg=message h.a=1",
         R"({"msg":"message","h":{"a":1}})"},
        {"nested empty group", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Group("g", slog::Group("h", slog::Group("i"), slog::Group("j")))}), "msg=message",
         R"({"msg":"message"})"},
        {"nested non-empty group", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Group("g", slog::Group("h", slog::Group("i"), slog::Group("j", slog::Int("a", 1))))}),
         "msg=message g.h.j.a=1", R"({"msg":"message","g":{"h":{"j":{"a":1}}}})"},
        {"escapes", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::String("a b", std::string("x\t\n\0y", 5)),
                    slog::Group(" b.c=\"\\x2E\t", slog::String("d=e", "f.g\""), slog::Int("m.d", 1))}),  // dot is not escaped
         R"(msg=message "a b"="x\t\n\x00y" " b.c=\"\\x2E\t.d=e"="f.g\"" " b.c=\"\\x2E\t.m.d"=1)",
         R"({"msg":"message","a b":"x\t\n\u0000y"," b.c=\"\\x2E\t":{"d=e":"f.g\"","m.d":1}})"},
        {"LogValuer", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Int("a", 1), slog::Any("name", logValueName{"Ren", "Hoek"}), slog::Int("b", 2)}),
         "msg=message a=1 name.first=Ren name.last=Hoek b=2",
         R"({"msg":"message","a":1,"name":{"first":"Ren","last":"Hoek"},"b":2})"},
        // Test resolution when there is no ReplaceAttr function.
        {"resolve", nullptr, false, nullptr,
         withAttrs({slog::Any("", make_ptr<replace>(Value())),  // should be elided
                    slog::Any("name", logValueName{"Ren", "Hoek"})}),
         "time=2000-01-02T03:04:05.000Z level=INFO msg=message name.first=Ren name.last=Hoek",
         R"({"time":"2000-01-02T03:04:05Z","level":"INFO","msg":"message","name":{"first":"Ren","last":"Hoek"}})"},
        {"with-group", removeKeys({slog::TimeKey, slog::LevelKey}), false,
         [&](ptr<slog::Handler> h) { return h->WithAttrs(preAttrs)->WithGroup("s"); }, attrs,
         "msg=message pre=3 x=y s.a=one s.b=2", R"({"msg":"message","pre":3,"x":"y","s":{"a":"one","b":2}})"},
        {"preformatted with-groups", removeKeys({slog::TimeKey, slog::LevelKey}), false,
         [](ptr<slog::Handler> h) {
             return h->WithAttrs({{slog::Int("p1", 1)}})
                 ->WithGroup("s1")
                 ->WithAttrs({{slog::Int("p2", 2)}})
                 ->WithGroup("s2")
                 ->WithAttrs({{slog::Int("p3", 3)}});
         },
         attrs, "msg=message p1=1 s1.p2=2 s1.s2.p3=3 s1.s2.a=one s1.s2.b=2",
         R"({"msg":"message","p1":1,"s1":{"p2":2,"s2":{"p3":3,"a":"one","b":2}}})"},
        {"two with-groups", removeKeys({slog::TimeKey, slog::LevelKey}), false,
         [](ptr<slog::Handler> h) { return h->WithAttrs({{slog::Int("p1", 1)}})->WithGroup("s1")->WithGroup("s2"); },
         attrs, "msg=message p1=1 s1.s2.a=one s1.s2.b=2", R"({"msg":"message","p1":1,"s1":{"s2":{"a":"one","b":2}}})"},
        {"empty with-groups", removeKeys({slog::TimeKey, slog::LevelKey}), false,
         [](ptr<slog::Handler> h) { return h->WithGroup("x")->WithGroup("y"); }, {}, "msg=message",
         R"({"msg":"message"})"},
        {"empty with-groups, no non-empty attrs", removeKeys({slog::TimeKey, slog::LevelKey}), false,
         [](ptr<slog::Handler> h) { return h->WithGroup("x")->WithAttrs({{slog::Group("g")}})->WithGroup("y"); }, {},
         "msg=message", R"({"msg":"message"})"},
        {"one empty with-group", removeKeys({slog::TimeKey, slog::LevelKey}), false,
         [](ptr<slog::Handler> h) { return h->WithGroup("x")->WithAttrs({{slog::Int("a", 1)}})->WithGroup("y"); },
         withAttrs({slog::Group("g", slog::Group("h"))}), "msg=message x.a=1", R"({"msg":"message","x":{"a":1}})"},
        {"GroupValue as Attr value", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({Attr{"v", slog::AnyValue(slog::IntValue(3))}}), "msg=message v=3", R"({"msg":"message","v":3})"},
        {"byte slice", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Any("bs", slice<byte>{1, 2, 3, 4})}), R"(msg=message bs="\x01\x02\x03\x04")",
         R"({"msg":"message","bs":"AQIDBA=="})"},
        {"inline group", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Int("a", 1), slog::Group("", slog::Int("b", 2), slog::Int("c", 3)), slog::Int("d", 4)}),
         "msg=message a=1 b=2 c=3 d=4", R"({"msg":"message","a":1,"b":2,"c":3,"d":4})"},
        {"Source",
         [](std::span<const string> gs, Attr a) {
             if (a.Key == slog::SourceKey) {
                 slog::Source s = *a.Value.As<slog::Source>();
                 s.File = path::Base(s.File);
                 return slog::Any(a.Key, s);
             }
             return removeKeys({slog::TimeKey, slog::LevelKey})(gs, std::move(a));
         },
         true, nullptr, {}, "source=go_slog_test.cpp:$LINE msg=message",
         R"({"source":{"function":"$FUNC","file":"go_slog_test.cpp","line":$LINE},"msg":"message"})"},
        {"replace built-in with group",
         [](std::span<const string>, Attr a) {
             if (a.Key == slog::TimeKey) return slog::Group(string::fromStatic(slog::TimeKey), "mins", 3, "secs", 2);
             if (a.Key == slog::LevelKey) return Attr{};
             return a;
         },
         false, nullptr, {}, "time.mins=3 time.secs=2 msg=message", R"({"time":{"mins":3,"secs":2},"msg":"message"})"},
        {"replace empty", removeAll, false, nullptr, withAttrs({slog::Group("g", slog::Int("a", 1))}), "", "{}"},
        {"replace empty 1", removeAll, false,
         [](ptr<slog::Handler> h) { return h->WithGroup("g")->WithAttrs({{slog::Int("a", 1)}}); },
         withAttrs({slog::Group("h", slog::Int("b", 2))}), "", "{}"},
        {"replace empty 2", removeAll, false,
         [](ptr<slog::Handler> h) {
             return h->WithGroup("g")->WithAttrs({{slog::Int("a", 1)}})->WithGroup("h")->WithAttrs({{slog::Int("b", 2)}});
         },
         withAttrs({slog::Group("i", slog::Int("c", 3))}), "", "{}"},
        {"replace empty 3", removeAll, false, [](ptr<slog::Handler> h) { return h->WithGroup("g"); },
         withAttrs({slog::Int("a", 1)}), "", "{}"},
        {"replace empty inline", removeAll, false,
         [](ptr<slog::Handler> h) {
             return h->WithGroup("g")->WithAttrs({{slog::Int("a", 1)}})->WithGroup("h")->WithAttrs({{slog::Int("b", 2)}});
         },
         withAttrs({slog::Group("", slog::Int("c", 3))}), "", "{}"},
        {"replace partial empty attrs 1", removeKeys({slog::TimeKey, slog::LevelKey, slog::MessageKey, "a"}), false,
         [](ptr<slog::Handler> h) {
             return h->WithGroup("g")->WithAttrs({{slog::Int("a", 1)}})->WithGroup("h")->WithAttrs({{slog::Int("b", 2)}});
         },
         withAttrs({slog::Group("i", slog::Int("c", 3))}), "g.h.b=2 g.h.i.c=3", R"({"g":{"h":{"b":2,"i":{"c":3}}}})"},
        {"replace partial empty attrs 2", removeKeys({slog::TimeKey, slog::LevelKey, slog::MessageKey, "a", "b"}),
         false,
         [](ptr<slog::Handler> h) {
             return h->WithGroup("g")
                 ->WithAttrs({{slog::Int("a", 1)}})
                 ->WithAttrs({{slog::Int("n", 4)}})
                 ->WithGroup("h")
                 ->WithAttrs({{slog::Int("b", 2)}});
         },
         withAttrs({slog::Group("i", slog::Int("c", 3))}), "g.n=4 g.h.i.c=3", R"({"g":{"n":4,"h":{"i":{"c":3}}}})"},
        {"replace partial empty attrs 3", removeKeys({slog::TimeKey, slog::LevelKey, slog::MessageKey, "a", "c"}),
         false,
         [](ptr<slog::Handler> h) {
             return h->WithGroup("g")
                 ->WithAttrs({{slog::Int("x", 0)}})
                 ->WithAttrs({{slog::Int("a", 1)}})
                 ->WithAttrs({{slog::Int("n", 4)}})
                 ->WithGroup("h")
                 ->WithAttrs({{slog::Int("b", 2)}});
         },
         withAttrs({slog::Group("i", slog::Int("c", 3))}), "g.x=0 g.n=4 g.h.b=2", R"({"g":{"x":0,"n":4,"h":{"b":2}}})"},
        {"replace resolved group",
         [](std::span<const string> groups, Attr a) {
             if (a.Value.Kind() == slog::KindGroup) return Attr{"bad", slog::IntValue(1)};
             return removeKeys({slog::TimeKey, slog::LevelKey, slog::MessageKey})(groups, std::move(a));
         },
         false, nullptr, withAttrs({slog::Any("name", logValueName{"Perry", "Platypus"})}),
         "name.first=Perry name.last=Platypus", R"({"name":{"first":"Perry","last":"Platypus"}})"},
        {"group and key (or both) needs quoting", removeKeys({slog::TimeKey, slog::LevelKey}), false, nullptr,
         withAttrs({slog::Group("prefix", slog::String(" needs quoting ", "v"), slog::String("NotNeedsQuoting", "v")),
                    slog::Group("prefix needs quoting", slog::String(" needs quoting ", "v"),
                                slog::String("NotNeedsQuoting", "v"))}),
         R"(msg=message "prefix. needs quoting "=v prefix.NotNeedsQuoting=v "prefix needs quoting. needs quoting "=v "prefix needs quoting.NotNeedsQuoting"=v)",
         R"({"msg":"message","prefix":{" needs quoting ":"v","NotNeedsQuoting":"v"},"prefix needs quoting":{" needs quoting ":"v","NotNeedsQuoting":"v"}})"},
    };

    auto replaceAll = [](std::string s, std::string_view from, std::string_view to) {
        for (auto i = s.find(from); i != s.npos; i = s.find(from, i + to.size())) s.replace(i, from.size(), to);
        return s;
    };
    for (const auto& test : tests) {
        auto r = slog::NewRecord(testTime, slog::LevelInfo, "message", std::source_location::current());
        auto source = r.Source();
        ASSERT_TRUE(source.has_value()) << "source is nil";
        std::string line = std::to_string(source->Line);
        std::string function(source->Function.view());
        r.AddAttrs(test.attrs);
        auto buf = newBuffer();
        slog::HandlerOptions opts{.AddSource = test.addSource, .ReplaceAttr = test.replace};
        struct {
            std::string name;
            ptr<slog::Handler> h;
            std::string want;
        } handlers[] = {
            {"text", slog::NewTextHandler(buf, opts), test.wantText},
            {"json", slog::NewJSONHandler(buf, opts), test.wantJSON},
        };
        for (auto& handler : handlers) {
            auto h = handler.h;
            if (test.with) h = test.with(h);
            buf->reset();
            ASSERT_FALSE(h->Handle(nullptr, r)) << test.name;
            std::string want = replaceAll(replaceAll(handler.want, "$LINE", line), "$FUNC", function);
            std::string got(buf->view());
            if (!got.empty() && got.back() == '\n') got.pop_back();
            EXPECT_EQ(got, want) << test.name << "/" << handler.name;
        }
    }
}

TEST(GoSlog, HandlerEnabled) {
    auto levelVar = [](slog::Level l) {
        auto al = make_ptr<slog::LevelVar>();
        al->Set(l);
        return al;
    };
    struct {
        slog::leveler leveler;
        bool want;
    } tests[] = {
        {nullptr, true},
        {slog::LevelWarn, false},
        {make_ptr<slog::LevelVar>(), true},  // defaults to Info
        {levelVar(slog::LevelWarn), false},
        {slog::LevelDebug, true},
        {levelVar(slog::LevelDebug), true},
    };
    int i = 0;
    for (const auto& test : tests) {
        slog::detail::commonHandler h;
        h.opts.Level = test.leveler;
        EXPECT_EQ(h.enabled(slog::LevelInfo), test.want) << i++;
    }
}

TEST(GoSlog, JSONAndTextHandlersWithUnavailableSource) {
    // Verify that a missing source does not cause a panic, and that the
    // source is empty.
    auto buf = newBuffer();
    slog::HandlerOptions opts{.AddSource = true, .ReplaceAttr = removeKeys({slog::LevelKey})};
    struct {
        std::string name;
        ptr<slog::Handler> h;
        std::string want;
    } tests[] = {
        {"text", slog::NewTextHandler(buf, opts), "msg=message"},
        {"json", slog::NewJSONHandler(buf, opts), R"({"msg":"message"})"},
    };
    for (const auto& test : tests) {
        buf->reset();
        auto r = slog::NewRecord(time::Time(), slog::LevelInfo, "message", {});
        ASSERT_FALSE(test.h->Handle(context::Background(), r));
        EXPECT_EQ(strings::TrimSpace(string(buf->view())), test.want) << test.name;
    }
}

TEST(GoSlog, SecondWith) {
    // Verify that a second call to Logger.With does not corrupt the original.
    auto buf = newBuffer();
    auto h = slog::NewTextHandler(buf, {.ReplaceAttr = removeKeys({slog::TimeKey})});
    auto logger = slog::New(h)->With(slog::String("app", "playground"), slog::String("role", "tester"),
                                     slog::Int("data_version", 2));
    auto appLogger = logger->With("type", "log");  // this becomes type=met
    (void)logger->With("type", "metric");
    appLogger->Info("foo");
    EXPECT_EQ(strings::TrimSpace(string(buf->view())),
              "level=INFO msg=foo app=playground role=tester data_version=2 type=log");
}

TEST(GoSlog, ReplaceAttrGroups) {
    // Verify that ReplaceAttr is called with the correct groups.
    struct ga {
        std::string groups, key, val;
        bool operator==(const ga&) const = default;
    };
    std::vector<ga> got;
    auto h = slog::NewTextHandler(ptr<io::Writer>(&io::Discard()), {.ReplaceAttr = [&](std::span<const string> gs, Attr a) {
                                       std::string v(a.Value.String().view());
                                       if (a.Key == slog::TimeKey) v = "<now>";
                                       got.push_back({std::string(strings::Join(gs, ",").view()), std::string(a.Key.view()), v});
                                       return a;
                                   }});
    slog::New(h)
        ->With(slog::Int("a", 1))
        ->WithGroup("g1")
        ->With(slog::Int("b", 2))
        ->WithGroup("g2")
        ->With(slog::Int("c", 3), slog::Group("g3", slog::Int("d", 4)), slog::Int("e", 5))
        ->Info("m", slog::Int("f", 6), slog::Group("g4", slog::Int("h", 7)), slog::Int("i", 8));

    std::vector<ga> want = {
        {"", "a", "1"},       {"g1", "b", "2"},       {"g1,g2", "c", "3"}, {"g1,g2,g3", "d", "4"},
        {"g1,g2", "e", "5"},  {"", "time", "<now>"},  {"", "level", "INFO"}, {"", "msg", "m"},
        {"g1,g2", "f", "6"},  {"g1,g2,g4", "h", "7"}, {"g1,g2", "i", "8"},
    };
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < got.size(); i++) {
        EXPECT_EQ(got[i], want[i]) << i << ": got {" << got[i].groups << " " << got[i].key << " " << got[i].val << "}";
    }
}

TEST(GoSlog, WriteTimeRFC3339) {
    constexpr std::string_view rfc3339Millis = "2006-01-02T15:04:05.000Z07:00";
    for (const time::Time& tm : {
             time::Date(2000, time::January, 2, 3, 4, 5, 0, time::UTC),
             time::Date(2000, time::January, 2, 3, 4, 5, 400, time::Local),
             time::Date(2000, time::November, 12, 3, 4, 500, 50'000'000, time::UTC),
         }) {
        std::string got;
        slog::detail::appendRfc3339Millis(got, tm);
        EXPECT_EQ(got, tm.Format(rfc3339Millis).view());
    }
}

TEST(GoSlog, DiscardHandler) {
    auto ctx = context::Background();
    // Just ensure nothing goes wrong during normal usage.
    auto l = slog::New(slog::DiscardHandler);
    l->Info("msg", "a", 1, "b", 2);
    l->Debug("bg", slog::Int("a", 1), "b", 2);
    l->Warn("w", slog::Duration("dur", 3 * time::Second));
    l->Error("bad", "a", 1);
    l->Log(ctx, slog::LevelWarn + 1, "w", slog::Int("a", 1), slog::String("b", "two"));
    l->LogAttrs(ctx, slog::LevelInfo + 1, "a b c", slog::Int("a", 1), slog::String("b", "two"));
    l->Info("info", "a", std::vector<Attr>{slog::Int("i", 1)});
    l->Info("info", "a", slog::GroupValue(slog::Int("i", 1)));
}

// ---------------------------------------------------------------------------
// text_handler_test.go

TEST(GoSlog, TextHandler) {
    struct anon {
        int A, b;
    };
    struct {
        std::string name;
        Attr attr;
        std::string wantKey, wantVal;
    } tests[] = {
        {"unquoted", slog::Int("a", 1), "a", "1"},
        {"quoted", slog::String("x = y", R"(qu"o)"), R"("x = y")", R"("qu\"o")"},
        {"String method", slog::Any("name", name{"Ren", "Hoek"}), "name", R"("Hoek, Ren")"},
        {"struct", slog::Any("x", std::make_shared<anon>(anon{1, 2})), "x", R"("&{A:1 b:2}")"},
        {"TextMarshaler", slog::Any("t", text_{"abc"}), "t", R"("text{\"abc\"}")"},
        {"TextMarshaler escapes", slog::Any("t", text_{marshalerASCII}), "t", quote("text{" + quote(marshalerASCII) + "}")},
        {"TextMarshaler error", slog::Any("t", text_{""}), "t", R"("!ERROR:text: empty string")"},
        {"TextAppender", slog::Any("t", textAppend{"abc"}), "t", R"("textAppend{\"abc\"}")"},
        {"TextAppender error", slog::Any("t", textAppend{""}), "t", R"("!ERROR:textAppend: empty string")"},
        {"nil value", slog::Any("a", nullptr), "a", "<nil>"},
    };
    for (const auto& test : tests) {
        struct {
            std::string name;
            slog::HandlerOptions opts;
            std::string wantPrefix;
            std::function<std::string(std::string)> modKey;
        } options[] = {
            {"none", {}, R"(time=2000-01-02T03:04:05.000Z level=INFO msg="a message")", [](std::string s) { return s; }},
            {"replace",
             {.ReplaceAttr = upperCaseKey},
             R"(TIME=2000-01-02T03:04:05.000Z LEVEL=INFO MSG="a message")",
             [](std::string s) { return std::string(strings::ToUpper(s).view()); }},
        };
        for (const auto& opts : options) {
            auto buf = newBuffer();
            auto h = slog::NewTextHandler(buf, opts.opts);
            auto r = slog::NewRecord(testTime, slog::LevelInfo, "a message", {});
            r.AddAttrs(test.attr);
            ASSERT_FALSE(h->Handle(context::Background(), r));
            std::string got(buf->view());
            got.pop_back();  // Remove final newline.
            std::string want = opts.wantPrefix + " " + opts.modKey(test.wantKey) + "=" + test.wantVal;
            EXPECT_EQ(got, want) << test.name << "/" << opts.name;
        }
    }
}

TEST(GoSlog, TextHandlerPreformatted) {
    auto buf = newBuffer();
    ptr<slog::Handler> h = slog::NewTextHandler(buf, nullptr);
    h = h->WithAttrs({{slog::Duration("dur", time::Minute), slog::Bool("b", true)}});
    // Also test omitting time.
    auto r = slog::NewRecord(time::Time(), slog::Level(0) /* 0 Level is INFO */, "m", {});
    r.AddAttrs(slog::Int("a", 1));
    ASSERT_FALSE(h->Handle(context::Background(), r));
    std::string got(buf->view());
    got.pop_back();
    EXPECT_EQ(got, "level=INFO msg=m dur=1m0s b=true a=1");
}

TEST(GoSlog, NeedsQuoting) {
    struct {
        std::string in;
        bool want;
    } tests[] = {
        {"", true},         {"ab", false},  {"a=b", true}, {R"("ab")", true}, {"\a\b", true},
        {"a\tb", true},     {"µåπ", false}, {"a b", true}, {"badutf8\xF6", true},
    };
    for (const auto& test : tests) EXPECT_EQ(slog::detail::needsQuoting(test.in), test.want) << quote(test.in);
}

// ---------------------------------------------------------------------------
// json_handler_test.go

std::string jsonValueString(const Value& v) {
    slog::detail::commonHandler h;
    h.json = true;
    std::string buf;
    slog::detail::handleState s(&h, &buf, "");
    if (error err = s.appendJsonValue(v)) s.appendError(err);
    return buf;
}

template <typename T>
std::string marshalJSON(const T& x) {
    json::detail::encoder e;
    e.escapeHtml = false;
    json::detail::encode(e, x);
    EXPECT_FALSE(e.err) << e.err;
    return e.out;
}

TEST(GoSlog, JSONHandler) {
    struct {
        std::string name;
        slog::HandlerOptions opts;
        std::string want;
    } tests[] = {
        {"none", {}, R"({"time":"2000-01-02T03:04:05Z","level":"INFO","msg":"m","a":1,"m":{"b":2}})"},
        {"replace",
         {.ReplaceAttr = upperCaseKey},
         R"({"TIME":"2000-01-02T03:04:05Z","LEVEL":"INFO","MSG":"m","A":1,"M":{"b":2}})"},
    };
    for (const auto& test : tests) {
        auto buf = newBuffer();
        auto h = slog::NewJSONHandler(buf, test.opts);
        auto r = slog::NewRecord(testTime, slog::LevelInfo, "m", {});
        r.AddAttrs(slog::Int("a", 1), slog::Any("m", std::map<std::string, int>{{"b", 2}}));
        ASSERT_FALSE(h->Handle(context::Background(), r));
        std::string got(buf->view());
        got.pop_back();
        EXPECT_EQ(got, test.want) << test.name;
    }
}

TEST(GoSlog, JSONHandlerMarshalerEscaping) {
    auto buf = newBuffer();
    auto h = slog::NewJSONHandler(buf, nullptr);
    auto r = slog::NewRecord(testTime, slog::LevelInfo, "m", {});
    r.AddAttrs(slog::Any("m", jsonMarshaler{marshalerASCII}));
    ASSERT_FALSE(h->Handle(context::Background(), r));
    std::string got(buf->view());
    got.pop_back();
    ASSERT_TRUE(json::Valid(got)) << "output is not valid JSON: " << got;
    for (std::string escaped : {R"(\")", R"(\\)", R"(\t)", R"(\n)", R"(\u0000)"}) {
        EXPECT_NE(got.find(escaped), got.npos) << "output does not contain JSON escape " << escaped;
    }
    for (int i = 0; i < 0x20; i++) {
        EXPECT_EQ(got.find(static_cast<char>(i)), got.npos) << "output contains unescaped control character " << i;
    }
    struct {
        [[= json::tag("m")]] std::vector<std::string> M;
    } record;
    ASSERT_FALSE(json::Unmarshal(got, &record));
    ASSERT_EQ(record.M.size(), 1u);
    EXPECT_EQ(record.M[0], marshalerASCII);
}

TEST(GoSlog, AppendJSONValue) {
    // The JSON of a Value should always agree with json.Marshal.
    auto check = [](const auto& value) {
        EXPECT_EQ(jsonValueString(slog::AnyValue(value)), marshalJSON(value));
    };
    check(std::string("hello\r\n\t\a"));
    check(std::string(R"("[{escape}]")"));
    check(std::string("<escapeHTML&>"));
    // \u2028\u2029 is an edge case in JavaScript vs JSON.
    // \xF6 is an incomplete encoding.
    check(std::string("\u03B8\u2028\u2029\uFFFF\xF6"));
    check(std::string("-123"));
    check(int64(-9'200'123'456'789'123'456));
    check(uint64(9'200'123'456'789'123'456u));
    check(-12.75);
    check(1.23e-9);
    check(false);
    check(time::Minute);
    check(testTime);
    check(jsonMarshaler{"xyz"});
    check(jsonMarshalerError{jsonMarshaler{"pqr"}});
    check(slog::LevelWarn);
}

TEST(GoSlog, JSONAppendAttrValueSpecial) {
    // Attr values that render differently from json.Marshal.
    struct {
        Value value;
        std::string want;
    } tests[] = {
        {slog::AnyValue(std::nan("")), R"("!ERROR:json: unsupported value: NaN")"},
        {slog::AnyValue(std::numeric_limits<double>::infinity()), R"("!ERROR:json: unsupported value: +Inf")"},
        {slog::AnyValue(-std::numeric_limits<double>::infinity()), R"("!ERROR:json: unsupported value: -Inf")"},
        {slog::AnyValue(io::EOF_), R"("EOF")"},
    };
    for (const auto& test : tests) EXPECT_EQ(jsonValueString(test.value), test.want) << test.value;
}

// ---------------------------------------------------------------------------
// logger_test.go

TEST(GoSlog, LogTextHandler) {
    auto ctx = context::Background();
    auto buf = newBuffer();
    auto l = slog::New(slog::NewTextHandler(buf, nullptr));

    auto check = [&](std::string want) {
        if (!want.empty()) want = "time=" + textTimeRE + " " + want;
        checkLogOutput(buf->view(), want);
        buf->reset();
    };

    l->Info("msg", "a", 1, "b", 2);
    check("level=INFO msg=msg a=1 b=2");

    // By default, debug messages are not printed.
    l->Debug("bg", slog::Int("a", 1), "b", 2);
    check("");

    l->Warn("w", slog::Duration("dur", 3 * time::Second));
    check("level=WARN msg=w dur=3s");

    l->Error("bad", "a", 1);
    check("level=ERROR msg=bad a=1");

    l->Log(ctx, slog::LevelWarn + 1, "w", slog::Int("a", 1), slog::String("b", "two"));
    check(R"(level=WARN\+1 msg=w a=1 b=two)");

    l->LogAttrs(ctx, slog::LevelInfo + 1, "a b c", slog::Int("a", 1), slog::String("b", "two"));
    check(R"(level=INFO\+1 msg="a b c" a=1 b=two)");

    l->Info("info", "a", std::vector<Attr>{slog::Int("i", 1)});
    check("level=INFO msg=info a.i=1");

    l->Info("info", "a", slog::GroupValue(slog::Int("i", 1)));
    check("level=INFO msg=info a.i=1");
}

TEST(GoSlog, Connections) {
    auto logbuf = newBuffer(), slogbuf = newBuffer();

    // Revert any changes to the default logger, and restore it at the end.
    restoreDefaults restore;
    slog::SetDefault(slog::New(logDefaultHandler()));

    // The default slog.Logger's handler uses the log package's default output.
    log::SetOutput(logbuf);
    log::SetFlags(log::Lshortfile & ~log::LstdFlags);
    slog::Info("msg", "a", 1);
    checkLogOutput(logbuf->view(), R"(go_slog_test\.cpp:\d+: INFO msg a=1)");
    logbuf->reset();
    slog::Info("msg", "p", nullptr);
    checkLogOutput(logbuf->view(), R"(go_slog_test\.cpp:\d+: INFO msg p=<nil>)");
    logbuf->reset();
    ptr<strings::Builder> r;
    slog::Info("msg", "r", r);
    checkLogOutput(logbuf->view(), R"(go_slog_test\.cpp:\d+: INFO msg r=<nil>)");
    logbuf->reset();
    slog::Warn("msg", "b", 2);
    checkLogOutput(logbuf->view(), R"(go_slog_test\.cpp:\d+: WARN msg b=2)");
    logbuf->reset();
    slog::Error("msg", "err", io::EOF_, "c", 3);
    checkLogOutput(logbuf->view(), R"(go_slog_test\.cpp:\d+: ERROR msg err=EOF c=3)");

    // Levels below Info are not printed.
    logbuf->reset();
    slog::Debug("msg", "c", 3);
    checkLogOutput(logbuf->view(), "");

    {
        // wrap default handler: it should be possible to wrap the default
        // handler and get the right output. This works because the default
        // handler uses the location in the Record to get the source line.
        auto logger = slog::New(make_ptr<wrappingHandler>(slog::Default()->Handler()));
        logger->Info("msg", "d", 4);
        checkLogOutput(logbuf->view(), R"(go_slog_test\.cpp:\d+: INFO msg d=4)");
    }

    // Once slog.SetDefault is called, the direction is reversed: the default
    // log.Logger's output goes through the handler.
    slog::SetDefault(slog::New(slog::NewTextHandler(slogbuf, {.AddSource = true})));
    log::Print("msg2");
    checkLogOutput(slogbuf->view(), "time=" + textTimeRE + R"( level=INFO source=.*go_slog_test\.cpp:\d+"? msg=msg2)");

    // The default log.Logger always outputs at Info level.
    slogbuf->reset();
    slog::SetDefault(slog::New(slog::NewTextHandler(slogbuf, {.Level = slog::LevelWarn})));
    log::Print("should not appear");
    EXPECT_EQ(slogbuf->view(), "");

    // Setting log's output again breaks the connection.
    logbuf->reset();
    slogbuf->reset();
    log::SetOutput(logbuf);
    log::SetFlags(log::Lshortfile & ~log::LstdFlags);
    log::Print("msg3");
    checkLogOutput(logbuf->view(), R"(go_slog_test\.cpp:\d+: msg3)");
    EXPECT_EQ(slogbuf->view(), "");
}

TEST(GoSlog, Attrs) {
    auto l1 = slog::New(make_ptr<captureHandler>())->With("a", 1);
    auto l2 = slog::New(l1->Handler())->With("b", 2);
    l2->Info("m", "c", 3);
    auto h = dynamic_pointer_cast<captureHandler>(l2->Handler());
    ASSERT_TRUE(h);
    EXPECT_TRUE(attrsEqual(h->attrs, std::vector<Attr>{slog::Int("a", 1), slog::Int("b", 2)})) << str(h->attrs);
    EXPECT_TRUE(attrsEqual(attrsSlice(h->r), std::vector<Attr>{slog::Int("c", 3)}));
}

TEST(GoSlog, CallDepth) {
    auto ctx = context::Background();
    auto h = make_ptr<captureHandler>();
    const std::string wantFunc = std::source_location::current().function_name();

    auto check = [&](int wantLine) {
        auto got = h->r.Source();
        ASSERT_TRUE(got.has_value()) << "got nil source";
        EXPECT_EQ(got->Function, wantFunc);
        EXPECT_EQ(path::Base(got->File), "go_slog_test.cpp");
        EXPECT_EQ(got->Line, wantLine);
    };

    restoreDefaults restore;
    auto logger = slog::New(h);
    slog::SetDefault(logger);

    // Each call is on the line of its check.
    logger->Log(ctx, slog::LevelInfo, ""); check(__LINE__);
    logger->LogAttrs(ctx, slog::LevelInfo, ""); check(__LINE__);
    logger->Debug(""); check(__LINE__);
    logger->Info(""); check(__LINE__);
    logger->Warn(""); check(__LINE__);
    logger->Error(""); check(__LINE__);
    slog::Debug(""); check(__LINE__);
    slog::Info(""); check(__LINE__);
    slog::Warn(""); check(__LINE__);
    slog::Error(""); check(__LINE__);
    slog::Log(ctx, slog::LevelInfo, ""); check(__LINE__);
    slog::LogAttrs(ctx, slog::LevelInfo, ""); check(__LINE__);
}

TEST(GoSlogDeathTest, CallDepthConnection) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // In the child: log through a text handler that keeps only the message
    // and the source.
    auto run = [](std::function<void()> f) {
        auto h = slog::NewTextHandler(ptr<io::Writer>(&os::Stderr),
                                      {.AddSource = true, .ReplaceAttr = [](std::span<const string> groups, Attr a) {
                                           if ((a.Key == slog::MessageKey || a.Key == slog::SourceKey) && groups.empty()) {
                                               return a;
                                           }
                                           return Attr{};
                                       }});
        slog::SetDefault(slog::New(h));
        log::SetFlags(log::Lshortfile);
        f();
        os::Exit(1);
    };
    auto want = [](int line, const char* name) {
        return std::format("^msg=\"go_slog_test\\.cpp:{}: {}\"\n", line, std::regex_replace(name, std::regex("\\."), "\\."));
    };
    int line = __LINE__;
    EXPECT_EXIT(run([] { log::Fatal("log.Fatal"); }), testing::ExitedWithCode(1), want(line + 1, "log.Fatal"));
    EXPECT_EXIT(run([] { log::Fatalln("log.Fatalln"); }), testing::ExitedWithCode(1), want(line + 2, "log.Fatalln"));
    EXPECT_EXIT(run([] { log::Output(1, "log.Output"); }), testing::ExitedWithCode(1), want(line + 3, "log.Output"));
    EXPECT_DEATH(run([] { log::Panic("log.Panic"); }), want(line + 4, "log.Panic"));
    EXPECT_DEATH(run([] { log::Panicln("log.Panicln"); }), want(line + 5, "log.Panicln"));
    EXPECT_EXIT(run([] { log::Default()->Fatal("log.Default.Fatal"); }), testing::ExitedWithCode(1), want(line + 6, "log.Default.Fatal"));
    EXPECT_EXIT(run([] { log::Default()->Fatalln("log.Default.Fatalln"); }), testing::ExitedWithCode(1), want(line + 7, "log.Default.Fatalln"));
    EXPECT_EXIT(run([] { log::Default()->Output(1, "log.Default.Output"); }), testing::ExitedWithCode(1), want(line + 8, "log.Default.Output"));
    EXPECT_DEATH(run([] { log::Default()->Panic("log.Default.Panic"); }), want(line + 9, "log.Default.Panic"));
    EXPECT_DEATH(run([] { log::Default()->Panicln("log.Default.Panicln"); }), want(line + 10, "log.Default.Panicln"));
}

TEST(GoSlog, SetAttrs) {
    auto check = [](std::vector<Attr> want, auto&&... args) {
        auto r = slog::NewRecord(time::Time(), slog::Level(0), "", {});
        r.Add(args...);
        auto got = attrsSlice(r);
        EXPECT_TRUE(attrsEqual(got, want)) << "got  " << str(got) << "\nwant " << str(want);
    };
    const string badKey = "!BADKEY";
    check({});
    check({slog::Int("a", 1)}, "a", 1);
    check({slog::Int("a", 1), slog::String("b", "two")}, "a", 1, "b", "two");
    check({slog::String(badKey, "a")}, "a");
    check({slog::Int("a", 1), slog::String(badKey, "b")}, "a", 1, "b");
    check({slog::Int("a", 1), slog::Int(badKey, 2), slog::Int(badKey, 3)}, "a", 1, 2, 3);
}

TEST(GoSlog, SetDefault) {
    // Verify that setting the default to itself does not result in deadlock.
    auto [ctx, cancel] = context::WithTimeout(context::Background(), time::Second);
    restoreDefaults restore;
    log::SetOutput(ptr<io::Writer>(&io::Discard()));
    go([cancel] {
        slog::Info("A");
        slog::SetDefault(slog::Default());
        slog::Info("B");
        cancel();
    });
    ctx->Done().recv();
    EXPECT_EQ(ctx->Err(), context::Canceled) << "wanted canceled, got " << ctx->Err();
    cancel();
}

// Test defaultHandler minimum level without calling slog.SetDefault.
TEST(GoSlog, LogLoggerLevelForDefaultHandler) {
    restoreDefaults restore;
    slog::SetDefault(slog::New(logDefaultHandler()));
    auto logBuf = newBuffer();
    log::SetOutput(logBuf);
    log::SetFlags(0);

    using logFunc = void (*)(slog::message);
    struct {
        slog::Level logLevel;
        logFunc logFn;
        std::string want;
    } tests[] = {
        {slog::LevelDebug, [](slog::message m) { slog::Debug(m); }, "DEBUG a"},
        {slog::LevelDebug, [](slog::message m) { slog::Info(m); }, "INFO a"},
        {slog::LevelInfo, [](slog::message m) { slog::Debug(m); }, ""},
        {slog::LevelInfo, [](slog::message m) { slog::Info(m); }, "INFO a"},
    };
    for (const auto& test : tests) {
        slog::SetLogLoggerLevel(test.logLevel);
        test.logFn("a");
        checkLogOutput(logBuf->view(), test.want);
        logBuf->reset();
    }
}

// Test handlerWriter minimum level by calling slog.SetDefault.
TEST(GoSlog, LogLoggerLevelForHandlerWriter) {
    auto removeTime = [](std::span<const string>, Attr a) {
        if (a.Key == slog::TimeKey) return Attr{};
        return a;
    };
    restoreDefaults restore;
    auto logBuf = newBuffer();
    log::SetOutput(logBuf);
    log::SetFlags(0);
    slog::SetLogLoggerLevel(slog::LevelError);
    slog::SetDefault(slog::New(slog::NewTextHandler(logBuf, {.ReplaceAttr = removeTime})));
    log::Print("error");
    checkLogOutput(logBuf->view(), "level=ERROR msg=error");
}

TEST(GoSlog, LoggerError) {
    auto buf = newBuffer();
    auto removeTime = [](std::span<const string>, Attr a) {
        if (a.Key == slog::TimeKey) return Attr{};
        return a;
    };
    auto l = slog::New(slog::NewTextHandler(buf, {.ReplaceAttr = removeTime}));
    l->Error("msg", "err", io::EOF_, "a", 1);
    checkLogOutput(buf->view(), "level=ERROR msg=msg err=EOF a=1");
    buf->reset();
    l->Error("msg", "err", io::EOF_, "a");
    checkLogOutput(buf->view(), "level=ERROR msg=msg err=EOF !BADKEY=a");
}

TEST(GoSlog, NewLogLogger) {
    auto buf = newBuffer();
    auto h = slog::NewTextHandler(buf, nullptr);
    auto ll = slog::NewLogLogger(h, slog::LevelWarn);
    ll->Print("hello");
    checkLogOutput(buf->view(), "time=" + textTimeRE + " level=WARN msg=hello");
}

TEST(GoSlog, LoggerNoOps) {
    auto l = slog::Default();
    EXPECT_EQ(l->With(), l) << "wanted receiver, didn't get it";
    EXPECT_EQ(slog::With(), l) << "wanted receiver, didn't get it";
    EXPECT_EQ(l->WithGroup(""), l) << "wanted receiver, didn't get it";
}

struct levelBox : object {
    explicit levelBox(slog::Level l) : l(l) {}
    slog::Level l;
};

TEST(GoSlog, Context) {
    // Verify that the context argument to log output methods is passed to the handler.
    // Also check the level.
    auto h = make_ptr<captureHandler>();
    auto l = slog::New(h);
    restoreDefaults restore;
    slog::SetDefault(l);

    using logFunc = std::function<void(const ptr<context::Context>&, slog::message)>;
    struct {
        logFunc f;
        slog::Level wantLevel;
    } tests[] = {
        {[&](const ptr<context::Context>& c, slog::message m) { l->DebugContext(c, m); }, slog::LevelDebug},
        {[&](const ptr<context::Context>& c, slog::message m) { l->InfoContext(c, m); }, slog::LevelInfo},
        {[&](const ptr<context::Context>& c, slog::message m) { l->WarnContext(c, m); }, slog::LevelWarn},
        {[&](const ptr<context::Context>& c, slog::message m) { l->ErrorContext(c, m); }, slog::LevelError},
        {[](const ptr<context::Context>& c, slog::message m) { slog::DebugContext(c, m); }, slog::LevelDebug},
        {[](const ptr<context::Context>& c, slog::message m) { slog::InfoContext(c, m); }, slog::LevelInfo},
        {[](const ptr<context::Context>& c, slog::message m) { slog::WarnContext(c, m); }, slog::LevelWarn},
        {[](const ptr<context::Context>& c, slog::message m) { slog::ErrorContext(c, m); }, slog::LevelError},
    };
    static const char key = 'L';
    for (const auto& test : tests) {
        h->clear();
        auto ctx = context::WithValue(context::Background(), &key, make_ptr<levelBox>(test.wantLevel));
        test.f(ctx, "msg");
        auto gv = dynamic_pointer_cast<levelBox>(h->ctx->Value(&key));
        ASSERT_TRUE(gv);
        EXPECT_EQ(gv->l, test.wantLevel);
        EXPECT_EQ(h->r.Level, test.wantLevel);
    }
}

// A type with MarshalText and MarshalJSON members, logged through a nil
// pointer. (Go's test also checks that panics in them are recovered.)
struct panicTextAndJsonMarshaler : object {
    std::pair<slice<byte>, error> MarshalText() const { panic("MarshalText"); }
    std::pair<slice<byte>, error> MarshalJSON() const { panic("MarshalJSON"); }
};

TEST(GoSlog, Panics) {
    restoreDefaults restore;
    auto logBuf = newBuffer();
    log::SetOutput(logBuf);
    log::SetFlags(log::Lshortfile & ~log::LstdFlags);

    slog::SetDefault(slog::New(logDefaultHandler()));
    slog::Info("msg", "p", ptr<panicTextAndJsonMarshaler>());
    checkLogOutput(logBuf->view(), R"(go_slog_test\.cpp:\d+: INFO msg p=<nil>)");
    logBuf->reset();

    slog::SetDefault(slog::New(slog::NewJSONHandler(logBuf, nullptr)));
    slog::Info("msg", "p", ptr<panicTextAndJsonMarshaler>());
    checkLogOutput(logBuf->view(), R"(\{"time":")" + jsonTimeRE + R"(","level":"INFO","msg":"msg","p":null\})");
}

// ---------------------------------------------------------------------------
// multi_handler_test.go

// mockFailingHandler is a handler that always returns an error from its
// Handle method.
struct mockFailingHandler : slog::Handler {
    mockFailingHandler(ptr<slog::Handler> h, error err) : h(std::move(h)), err(std::move(err)) {}
    ptr<slog::Handler> h;
    error err;
    bool Enabled(const ptr<context::Context>& ctx, slog::Level level) override { return h->Enabled(ctx, level); }
    error Handle(const ptr<context::Context>& ctx, const slog::Record& r) override {
        (void)h->Handle(ctx, r);
        return err;
    }
    ptr<slog::Handler> WithAttrs(std::span<const Attr> as) override { return h->WithAttrs(as); }
    ptr<slog::Handler> WithGroup(std::string_view name) override { return h->WithGroup(name); }
};

TEST(GoSlog, MultiHandler) {
    {
        // Handle sends log to all handlers
        auto buf1 = newBuffer(), buf2 = newBuffer();
        auto h1 = slog::NewTextHandler(buf1, nullptr);
        auto h2 = slog::NewJSONHandler(buf2, nullptr);
        auto multi = slog::NewMultiHandler(h1, h2);
        auto logger = slog::New(multi);
        logger->Info("hello world", "user", "test");
        checkLogOutput(buf1->view(), "time=" + textTimeRE + R"( level=INFO msg="hello world" user=test)");
        checkLogOutput(buf2->view(), R"(\{"time":")" + jsonTimeRE + R"(","level":"INFO","msg":"hello world","user":"test"\})");
    }
    {
        // Enabled returns true if any handler is enabled
        auto h1 = slog::NewTextHandler(newBuffer(), {.Level = slog::LevelError});
        auto h2 = slog::NewTextHandler(newBuffer(), {.Level = slog::LevelInfo});
        auto multi = slog::NewMultiHandler(h1, h2);
        EXPECT_TRUE(multi->Enabled(context::Background(), slog::LevelInfo));
        EXPECT_TRUE(multi->Enabled(context::Background(), slog::LevelError));
    }
    {
        // Enabled returns false if no handlers are enabled
        auto h1 = slog::NewTextHandler(newBuffer(), {.Level = slog::LevelError});
        auto h2 = slog::NewTextHandler(newBuffer(), {.Level = slog::LevelInfo});
        auto multi = slog::NewMultiHandler(h1, h2);
        EXPECT_FALSE(multi->Enabled(context::Background(), slog::LevelDebug));
    }
    {
        // WithAttrs propagates attributes to all handlers
        auto buf1 = newBuffer(), buf2 = newBuffer();
        auto h1 = slog::NewTextHandler(buf1, nullptr);
        auto h2 = slog::NewJSONHandler(buf2, nullptr);
        auto multi = slog::NewMultiHandler(h1, h2)->WithAttrs({{slog::String("request_id", "123")}});
        auto logger = slog::New(multi);
        logger->Info("request processed");
        checkLogOutput(buf1->view(), "time=" + textTimeRE + R"( level=INFO msg="request processed" request_id=123)");
        checkLogOutput(buf2->view(),
                       R"(\{"time":")" + jsonTimeRE + R"(","level":"INFO","msg":"request processed","request_id":"123"\})");
    }
    {
        // WithGroup propagates group to all handlers
        auto buf1 = newBuffer(), buf2 = newBuffer();
        auto h1 = slog::NewTextHandler(buf1, {.AddSource = false});
        auto h2 = slog::NewJSONHandler(buf2, {.AddSource = false});
        auto multi = slog::NewMultiHandler(h1, h2)->WithGroup("req");
        auto logger = slog::New(multi);
        logger->Info("user login", "user_id", 42);
        checkLogOutput(buf1->view(), "time=" + textTimeRE + R"( level=INFO msg="user login" req.user_id=42)");
        checkLogOutput(buf2->view(), R"(\{"time":")" + jsonTimeRE + R"(","level":"INFO","msg":"user login","req":\{"user_id":42\}\})");
    }
    {
        // Handle propagates errors from handlers
        auto errFail = errors::New("mock failing");
        auto buf1 = newBuffer(), buf2 = newBuffer();
        auto h1 = slog::NewTextHandler(buf1, nullptr);
        auto h2 = make_ptr<mockFailingHandler>(slog::NewJSONHandler(buf2, nullptr), errFail);
        auto multi = slog::NewMultiHandler(h2, h1);
        auto err = multi->Handle(context::Background(), slog::NewRecord(time::Now(), slog::LevelInfo, "test message", {}));
        EXPECT_TRUE(errors::Is(err, errFail)) << "Expected error: " << errFail << ", but got: " << err;
        checkLogOutput(buf1->view(), "time=" + textTimeRE + R"( level=INFO msg="test message")");
        checkLogOutput(buf2->view(), R"(\{"time":")" + jsonTimeRE + R"(","level":"INFO","msg":"test message"\})");
    }
    {
        // Handle with no handlers
        auto multi = slog::NewMultiHandler();
        auto logger = slog::New(multi);
        logger->Info("nothing");
        auto err = multi->Handle(context::Background(), slog::NewRecord(time::Now(), slog::LevelInfo, "test", {}));
        EXPECT_FALSE(err) << "Handle with no sub-handlers should return nil, but got: " << err;
    }
}

// Test that NewMultiHandler copies the input slice and is insulated from
// future modification.
TEST(GoSlog, NewMultiHandlerCopy) {
    auto buf1 = newBuffer();
    auto h1 = slog::NewTextHandler(buf1, nullptr);
    std::vector<ptr<slog::Handler>> handlers = {h1};
    auto multi = slog::NewMultiHandler(std::span<const ptr<slog::Handler>>(handlers));
    handlers[0] = nullptr;
    auto err = multi->Handle(context::Background(), slog::NewRecord(time::Now(), slog::LevelInfo, "test message", {}));
    EXPECT_FALSE(err) << "Expected nil error, but got: " << err;
    checkLogOutput(buf1->view(), "time=" + textTimeRE + R"( level=INFO msg="test message")");
}

}  // namespace
