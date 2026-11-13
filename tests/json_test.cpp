#include <gopp/chan.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

using namespace gopp;
using namespace std::chrono_literals;

namespace {

template <typename T>
std::string marshal_str(const T& v) {
    auto [data, err] = json::Marshal(v);
    EXPECT_FALSE(err) << err;
    return std::string(as_string_view(data));
}

struct point {
    int x;
    int y;
};

struct item {
    string name;
    [[=json::tag("n,omitempty")]] int64 count;
    [[=json::tag("-")]] string secret;
    [[=json::tag(",omitempty")]] std::vector<string> labels;
    std::optional<point> where;
    bool ok;
};

struct quoted {
    [[=json::tag("id,string")]] int64 id;
    [[=json::tag(",string")]] bool flag;
};

struct nested {
    string title;
    std::vector<point> points;
    std::map<string, int> counts;
    std::shared_ptr<item> extra;  // Go's *item
};

struct boxed final : object {
    int v = 0;
};

struct with_box {
    ptr<boxed> box;
};

// A type that encodes itself, like a json.Marshaler.
struct celsius {
    double deg;
    std::pair<bytes, error> MarshalJSON() const { return {to_bytes(fmt_deg()), nullptr}; }
    error UnmarshalJSON(std::span<const byte> d) {
        std::string_view s = as_string_view(d);
        if (s.size() < 3 || s.front() != '"' || !s.ends_with("C\"")) return errors::New("bad temperature");
        deg = std::stod(std::string(s.substr(1, s.size() - 3)));
        return nullptr;
    }
    std::string fmt_deg() const { return "\"" + std::to_string(static_cast<int>(deg)) + "C\""; }
};

}  // namespace

TEST(Json, Primitives) {
    EXPECT_EQ(marshal_str(true), "true");
    EXPECT_EQ(marshal_str(42), "42");
    EXPECT_EQ(marshal_str(-7L), "-7");
    EXPECT_EQ(marshal_str(uint64(18446744073709551615ULL)), "18446744073709551615");
    EXPECT_EQ(marshal_str(string("hi")), "\"hi\"");
    EXPECT_EQ(marshal_str(std::string("hi")), "\"hi\"");
    EXPECT_EQ(marshal_str(nullptr), "null");
}

TEST(Json, Floats) {
    // The same output as Go's float encoder.
    EXPECT_EQ(marshal_str(1.0), "1");
    EXPECT_EQ(marshal_str(1.5), "1.5");
    EXPECT_EQ(marshal_str(0.1), "0.1");
    EXPECT_EQ(marshal_str(1e21), "1e+21");
    EXPECT_EQ(marshal_str(1e20), "100000000000000000000");
    EXPECT_EQ(marshal_str(0.000001), "0.000001");
    EXPECT_EQ(marshal_str(1e-7), "1e-7");
    EXPECT_EQ(marshal_str(-2.5e-10), "-2.5e-10");
    EXPECT_EQ(marshal_str(3.0f), "3");
    EXPECT_EQ(marshal_str(0.1f), "0.1");
    auto [d, err] = json::Marshal(std::nan(""));
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "json: unsupported value: NaN");
    EXPECT_EQ(json::Marshal(std::numeric_limits<double>::infinity()).second.Error(), "json: unsupported value: +Inf");
}

TEST(Json, StringEscapes) {
    EXPECT_EQ(marshal_str(std::string("a\"b\\c")), R"("a\"b\\c")");
    EXPECT_EQ(marshal_str(std::string("\n\r\t\b\f")), R"("\n\r\t\b\f")");
    EXPECT_EQ(marshal_str(std::string("\x01")), "\"\\u0001\"");
    EXPECT_EQ(marshal_str(std::string("<a&b>")), "\"\\u003ca\\u0026b\\u003e\"");
    EXPECT_EQ(marshal_str(std::string("caf\xc3\xa9")), "\"caf\xc3\xa9\"");
    EXPECT_EQ(marshal_str(std::string("\xe2\x80\xa8")), "\"\\u2028\"");
    EXPECT_EQ(marshal_str(std::string("bad\xff")), "\"bad\\ufffd\"");
}

TEST(Json, Structs) {
    item it{.name = "x", .count = 3, .secret = "s", .labels = {}, .where = std::nullopt, .ok = true};
    EXPECT_EQ(marshal_str(it), R"({"name":"x","n":3,"where":null,"ok":true})");
    it.count = 0;
    it.labels = {"a", "b"};
    it.where = point{1, 2};
    EXPECT_EQ(marshal_str(it), R"({"name":"x","labels":["a","b"],"where":{"x":1,"y":2},"ok":true})");
    EXPECT_EQ(marshal_str(quoted{12, true}), R"({"id":"12","flag":"true"})");
}

TEST(Json, Containers) {
    EXPECT_EQ(marshal_str(std::vector<int>{1, 2, 3}), "[1,2,3]");
    EXPECT_EQ(marshal_str(std::vector<int>{}), "[]");
    EXPECT_EQ(marshal_str(slice<int>()), "null");  // nil slice
    EXPECT_EQ(marshal_str(slice<int>::make(0)), "[]");
    EXPECT_EQ(marshal_str(std::array<int, 2>{4, 5}), "[4,5]");
    EXPECT_EQ(marshal_str(to_bytes("hello")), R"("aGVsbG8=")");  // []byte is base64
    std::map<string, int> m{{"b", 2}, {"a", 1}};
    EXPECT_EQ(marshal_str(m), R"({"a":1,"b":2})");
    std::unordered_map<int, string> im{{10, "ten"}, {9, "nine"}};
    EXPECT_EQ(marshal_str(im), R"({"10":"ten","9":"nine"})");  // sorted as strings, like Go
    EXPECT_EQ(marshal_str(std::optional<int>()), "null");
    EXPECT_EQ(marshal_str(ptr<boxed>()), "null");
    auto b = make_ptr<boxed>();
    b->v = 5;
    EXPECT_EQ(marshal_str(with_box{b}), R"({"box":{"v":5}})");
}

TEST(Json, TimeAndDuration) {
    EXPECT_EQ(marshal_str(time::Time::Unix(1700000000, 500000000).UTC()), R"("2023-11-14T22:13:20.5Z")");
    EXPECT_EQ(marshal_str(time::Time::Unix(1700000000, 0).In(time::FixedZone("", 3600))), R"("2023-11-14T23:13:20+01:00")");
    EXPECT_EQ(marshal_str(1500 * time::Millisecond), "1500000000");
    time::Time t;
    ASSERT_FALSE(json::Unmarshal(R"("2023-11-14T23:13:20.25+01:00")", &t));
    EXPECT_EQ(t.Unix(), 1700000000);
    EXPECT_EQ(t.UnixNano() % 1000000000, 250000000);
}

TEST(Json, MarshalerAndRawMessage) {
    EXPECT_EQ(marshal_str(celsius{21}), R"("21C")");
    celsius c{};
    ASSERT_FALSE(json::Unmarshal(R"("30C")", &c));
    EXPECT_EQ(c.deg, 30);
    EXPECT_TRUE(json::Unmarshal(R"("hot")", &c));

    json::RawMessage raw{to_bytes(R"({ "a" : [1, 2] })")};
    EXPECT_EQ(marshal_str(raw), R"({"a":[1,2]})");  // compacted
    struct holder {
        json::RawMessage payload;
    } h;
    ASSERT_FALSE(json::Unmarshal(R"({"payload": {"x": true}})", &h));
    EXPECT_EQ(as_string_view(h.payload.data), R"({"x": true})");
}

TEST(Json, MarshalIndent) {
    nested n{"t", {{1, 2}}, {{"a", 1}}, nullptr};
    auto [data, err] = json::MarshalIndent(n, "", "  ");
    ASSERT_FALSE(err);
    EXPECT_EQ(as_string_view(data), R"({
  "title": "t",
  "points": [
    {
      "x": 1,
      "y": 2
    }
  ],
  "counts": {
    "a": 1
  },
  "extra": null
})");
    EXPECT_EQ(as_string_view(json::MarshalIndent(std::vector<int>{}, ">", "\t").first), "[]");
}

TEST(Json, UnmarshalStruct) {
    item it{};
    it.secret = "kept";
    auto err = json::Unmarshal(R"({"name":"x","N":7,"secret":"no","labels":["a"],"where":{"x":3,"y":4},"ok":true,"other":1})", &it);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(it.name, "x");
    EXPECT_EQ(it.count, 7);          // matched case-insensitively
    EXPECT_EQ(it.secret, "kept");    // "-" fields are never decoded
    ASSERT_EQ(it.labels.size(), 1u);
    ASSERT_TRUE(it.where);
    EXPECT_EQ(it.where->y, 4);
    EXPECT_TRUE(it.ok);

    nested n{};
    ASSERT_FALSE(json::Unmarshal(R"({"title":"t","points":[{"x":1,"y":2},{"x":3,"y":4}],"counts":{"a":1,"b":2},"extra":{"name":"e"}})", &n));
    EXPECT_EQ(n.points.size(), 2u);
    EXPECT_EQ(n.counts["b"], 2);
    ASSERT_TRUE(n.extra);
    EXPECT_EQ(n.extra->name, "e");

    quoted q{};
    ASSERT_FALSE(json::Unmarshal(R"({"id":"99","flag":"true"})", &q));
    EXPECT_EQ(q.id, 99);
    EXPECT_TRUE(q.flag);
}

TEST(Json, UnmarshalValues) {
    std::vector<double> v;
    ASSERT_FALSE(json::Unmarshal("[1, 2.5, -3e2]", &v));
    EXPECT_EQ(v, (std::vector<double>{1, 2.5, -300}));
    string s;
    ASSERT_FALSE(json::Unmarshal("\"a\\u00e9\\ud83d\\ude00\\n\"", &s));  // \u escapes and a surrogate pair
    EXPECT_EQ(s, "a\xc3\xa9\xf0\x9f\x98\x80\n");
    bytes b;
    ASSERT_FALSE(json::Unmarshal(R"("aGVsbG8=")", &b));
    EXPECT_EQ(as_string_view(b), "hello");
    std::map<int, bool> m;
    ASSERT_FALSE(json::Unmarshal(R"({"1":true,"2":false})", &m));
    EXPECT_TRUE(m[1]);
    std::array<int, 3> a{9, 9, 9};
    ASSERT_FALSE(json::Unmarshal("[1]", &a));
    EXPECT_EQ(a, (std::array<int, 3>{1, 0, 0}));
    int keep = 5;
    ASSERT_FALSE(json::Unmarshal("null", &keep));
    EXPECT_EQ(keep, 5);  // null leaves non-pointer values alone, as in Go
}

TEST(Json, DynamicValue) {
    json::Value v;
    ASSERT_FALSE(json::Unmarshal(R"({"b":[1,"two",true,null],"a":{"x":1.5}})", &v));
    ASSERT_TRUE(v.IsObject());
    const json::Value& cv = v;  // reading through non-const [] would insert keys
    EXPECT_EQ(cv["a"]["x"].AsNumber(), 1.5);
    EXPECT_EQ(cv["b"].size(), 4u);
    EXPECT_EQ(cv["b"][1].AsString(), "two");
    EXPECT_TRUE(cv["b"][3].IsNull());
    EXPECT_TRUE(cv["missing"].IsNull());
    EXPECT_EQ(marshal_str(v), R"({"a":{"x":1.5},"b":[1,"two",true,null]})");

    json::Value built;
    built["name"] = "go++";
    built["tags"] = json::Value::array{1, "x"};
    EXPECT_EQ(marshal_str(built), R"({"name":"go++","tags":[1,"x"]})");
}

TEST(Json, Errors) {
    int n = 0;
    auto err = json::Unmarshal(R"("x")", &n);
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "json: cannot unmarshal string into C++ value of type int");
    EXPECT_TRUE(json::Unmarshal("1.5", &n));
    EXPECT_TRUE(json::Unmarshal("300", reinterpret_cast<int8*>(&n)));

    item it{};
    err = json::Unmarshal(R"({"name":1,"ok":true})", &it);
    ASSERT_TRUE(err);
    EXPECT_NE(std::string(err.Error().view()).find("struct field"), std::string::npos) << err;
    EXPECT_NE(std::string(err.Error().view()).find(".name of type"), std::string::npos) << err;
    EXPECT_TRUE(it.ok);  // decoding goes on after a type error

    err = json::Unmarshal(R"({"a":1,})", &n);
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "invalid character '}' looking for beginning of object key string");
    auto* se = errors::As<json::SyntaxError>(err);
    ASSERT_TRUE(se);
    EXPECT_EQ(se->offset, 8);
    EXPECT_EQ(json::Unmarshal("[1,2", &n).Error(), "unexpected end of JSON input");
    EXPECT_EQ(json::Unmarshal("tru", &n).Error(), "unexpected end of JSON input");
    EXPECT_EQ(json::Unmarshal("nul1", &n).Error(), "invalid character '1' in literal null (expecting 'l')");
    EXPECT_EQ(json::Unmarshal("1 2", &n).Error(), "invalid character '2' after top-level value");
    EXPECT_EQ(json::Unmarshal("\"a\nb\"", &n).Error(), "invalid character '\\n' in string literal");
    EXPECT_FALSE(json::Valid("{"));
    EXPECT_TRUE(json::Valid(R"({"a":[1,{"b":null}]})"));
    EXPECT_EQ(json::Unmarshal("1", static_cast<int*>(nullptr)).Error(), "json: Unmarshal(nil *int)");
}

TEST(Json, EncoderDecoder) {
    strings::Builder b;
    auto enc = json::NewEncoder(b);
    enc.Encode(point{1, 2});
    enc.SetEscapeHTML(false);
    enc.Encode(std::string("<b>"));
    EXPECT_EQ(b.view(), "{\"x\":1,\"y\":2}\n\"<b>\"\n");

    io::StringReader r(R"({"x":1,"y":2} {"x":3,"y":4}
[5] 6 "seven")");
    auto dec = json::NewDecoder(r);
    point p{};
    ASSERT_FALSE(dec.Decode(&p));
    EXPECT_EQ(p.x, 1);
    ASSERT_FALSE(dec.Decode(&p));
    EXPECT_EQ(p.y, 4);
    std::vector<int> v;
    ASSERT_FALSE(dec.Decode(&v));
    EXPECT_EQ(v, std::vector<int>{5});
    int six = 0;
    ASSERT_FALSE(dec.Decode(&six));
    EXPECT_EQ(six, 6);
    EXPECT_TRUE(dec.More());
    string seven;
    ASSERT_FALSE(dec.Decode(&seven));
    EXPECT_EQ(seven, "seven");
    EXPECT_FALSE(dec.More());
    EXPECT_EQ(dec.Decode(&seven), io::EOF_);

    io::StringReader r2(R"({"x":1,"z":2})");
    auto strict = json::NewDecoder(r2);
    strict.DisallowUnknownFields();
    EXPECT_EQ(strict.Decode(&p).Error(), "json: unknown field \"z\"");
}

TEST(Json, DeepNestingOnFiberStack) {
    // Hostile input nested up to Go's limit must not overflow a fiber's
    // stack (256 KiB by default): scanning, decoding into json::Value and
    // destroying it are iterative.
    std::string deep = strings::Repeat("[", 9999).str() + strings::Repeat("]", 9999).str();
    std::string too_deep = strings::Repeat("[", 3000000).str() + strings::Repeat("]", 3000000).str();
    auto done = make_chan<std::pair<error, error>>(1);
    go([&] {
        json::Value v;
        error e1 = json::Unmarshal(deep, &v);
        json::Value w;
        error e2 = json::Unmarshal(too_deep, &w);
        done.send({e1, e2});
    });
    auto [e1, e2] = done.recv();
    EXPECT_FALSE(e1) << e1;
    ASSERT_TRUE(e2);
    EXPECT_TRUE(strings::Contains(e2.Error(), "exceeded max depth")) << e2;
}
